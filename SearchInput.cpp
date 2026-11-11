#include "SearchInput.hpp"
#include "globals.hpp"

#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/keybinds/Manager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace {
CFunctionHook* keyboardHook = nullptr;
CFunctionHook* pointerHook = nullptr;
bool initialized = false;

struct SInputContext {
    SP<IKeyboard> keyboard;
    SP<IPointer> pointer;
    uint32_t code = 0;
    bool released = false;
    bool historicalRelease = false;
    bool syntheticRecord = false;
};

// A stack-local context keeps devices alive through synchronous listeners.
// Restoring the previous pointer also handles nested input dispatches.
thread_local SInputContext* inputContext = nullptr;

constexpr std::string_view SYNTHETIC_RELEASE_SUBMAP =
    "\x1fscrolloverview:synthetic-historical-release";

SP<IHID> sourceDevice(const SInputContext& context) {
    return context.keyboard ? SP<IHID>{context.keyboard}
                            : SP<IHID>{context.pointer};
}

Keybinds::SResolvedKey eventKey(const SInputContext& context) {
    Keybinds::SResolvedKey key;
    if (context.keyboard)
        key.code = context.code + 8;
    else if (context.pointer)
        key.event = "mouse:" + std::to_string(context.code);
    return key;
}

void cleanupSyntheticRecord(const SInputContext& context) {
    if (!context.syntheticRecord || !context.keyboard || !Keybinds::mgr())
        return;
    const auto device = sourceDevice(context);
    const auto key = eventKey(context);
    auto& state = Keybinds::mgr()->inputState();
    const auto* input = state.find(key, device);
    if (input && input->submapAtPress == SYNTHETIC_RELEASE_SUBMAP)
        state.release(key, device);
}

using TKeyboardKey = void (*)(CInputManager*, const IKeyboard::SKeyEvent&,
                             SP<IKeyboard>);
using TMouseButton = void (*)(CInputManager*, IPointer::SButtonEvent,
                             SP<IPointer>);

void keyboardKey(CInputManager* manager, const IKeyboard::SKeyEvent& event,
                 SP<IKeyboard> device) {
    const auto original =
        reinterpret_cast<TKeyboardKey>(keyboardHook->m_original);
    SInputContext current{
        .keyboard = device,
        .pointer = {},
        .code = event.keycode,
        .released = event.state == WL_KEYBOARD_KEY_STATE_RELEASED,
    };
    const auto previous = inputContext;
    Hyprutils::Utils::CScopeGuard restore([previous, &current] {
        inputContext = previous;
        cleanupSyntheticRecord(current);
    });
    inputContext = &current;
    original(manager, event, std::move(device));
}

void mouseButton(CInputManager* manager, IPointer::SButtonEvent event,
                 SP<IPointer> device) {
    const auto original =
        reinterpret_cast<TMouseButton>(pointerHook->m_original);
    SInputContext current{
        .keyboard = {},
        .pointer = device,
        .code = event.button,
        .released = event.state == WL_POINTER_BUTTON_STATE_RELEASED,
    };
    const auto previous = inputContext;
    Hyprutils::Utils::CScopeGuard restore([previous] {
        inputContext = previous;
    });
    inputContext = &current;
    original(manager, event, std::move(device));
}

void* findInputFunction(const std::string& name,
                        std::string_view signature) {
    void* result = nullptr;
    for (const auto& match :
         HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name)) {
        if (match.demangled.find(signature) == std::string::npos)
            continue;
        // Refuse ambiguous matches rather than hook an arbitrary overload.
        if (result || !match.address)
            return nullptr;
        result = match.address;
    }
    return result;
}

void removeHook(CFunctionHook*& hook) {
    if (!hook)
        return;
    // The API owns the hook and removes its trampoline as well as unhooking.
    if (HyprlandAPI::removeFunctionHook(SCROLLOVERVIEW_HANDLE, hook))
        hook = nullptr;
}
}

namespace OverviewSearchInput {
bool initialize() {
    if (initialized)
        return true;

    try {
        // Retry cleanup first if an earlier API removal did not succeed.
        shutdown();
        if (keyboardHook || pointerHook || !SCROLLOVERVIEW_HANDLE)
            return false;

        const auto keyAddress = findInputFunction(
            "onKeyboardKey",
            "CInputManager::onKeyboardKey(IKeyboard::SKeyEvent const&, "
            "Hyprutils::Memory::CSharedPointer<IKeyboard>)");
        const auto buttonAddress = findInputFunction(
            "onMouseButton",
            "CInputManager::onMouseButton(IPointer::SButtonEvent, "
            "Hyprutils::Memory::CSharedPointer<IPointer>)");
        if (!keyAddress || !buttonAddress)
            return false;

        keyboardHook = HyprlandAPI::createFunctionHook(
            SCROLLOVERVIEW_HANDLE, keyAddress,
            reinterpret_cast<void*>(keyboardKey));
        if (keyboardHook)
            pointerHook = HyprlandAPI::createFunctionHook(
                SCROLLOVERVIEW_HANDLE, buttonAddress,
                reinterpret_cast<void*>(mouseButton));

        if (keyboardHook && pointerHook && keyboardHook->hook() &&
            pointerHook->hook()) {
            initialized = true;
            return true;
        }
    } catch (...) {
        // Missing symbols and hook failures must not produce notifications.
    }
    shutdown();
    return false;
}

bool available() {
    return initialized;
}

void shutdown() {
    initialized = false;
    // Attempt both removals even if one API operation throws.
    try {
        removeHook(pointerHook);
    } catch (...) {
    }
    try {
        removeHook(keyboardHook);
    } catch (...) {
    }
    // Do not clear a live stack-local context: its wrapper restores it.
}

SP<IKeyboard> keyboard() {
    return inputContext ? inputContext->keyboard : SP<IKeyboard>{};
}

SP<IPointer> pointer() {
    return inputContext ? inputContext->pointer : SP<IPointer>{};
}

bool hasPressRecord() {
    if (!inputContext || !Keybinds::mgr())
        return false;
    const auto device = sourceDevice(*inputContext);
    return device && Keybinds::mgr()->inputState().find(
        eventKey(*inputContext), device);
}

void suppressHistoricalReleaseBinds() {
    if (!inputContext || !inputContext->released)
        return;

    inputContext->historicalRelease = true;
    const auto& manager = Keybinds::mgr();
    if (!manager)
        return;

    const auto device = sourceDevice(*inputContext);
    if (!device)
        return;
    const auto key = eventKey(*inputContext);
    auto& state = manager->inputState();
    auto* input = state.find(key, device);

    if (!input) {
        // Mouse releases without a record do not match release binds upstream.
        if (!inputContext->keyboard)
            return;

        // A real record prevents the upstream no-record release matcher from
        // running. It does not synthesize a seat press: m_pressed still decides
        // whether the input manager forwards the physical release to the seat.
        Keybinds::SPressedInput synthetic;
        synthetic.key = key;
        synthetic.forwarded = true;
        synthetic.actionCode = key.code;
        synthetic.submapAtPress = SYNTHETIC_RELEASE_SUBMAP;
        synthetic.device = device;
        for (const auto& bind : manager->registry().binds())
            synthetic.suppressedBinds.emplace_back(bind);

        // Arm cleanup before press(): it can insert the record and then throw
        // while rebuilding heldKeys. The wrapper only removes marked records.
        inputContext->syntheticRecord = true;
        if (!state.press(std::move(synthetic)))
            inputContext->syntheticRecord = false;
        return;
    }

    input->deferredBinds.clear();
    input->armedReleaseBinds.clear();
    // Keep releaseCallbacks: these finish actions already started by the
    // press. Do not remove the record or change its forwarded status.
    for (const auto& bind : manager->registry().binds()) {
        if (std::ranges::none_of(input->suppressedBinds,
                [&bind](const auto& weak) {
                    return weak.lock() == bind;
                }))
            input->suppressedBinds.emplace_back(bind);
    }
}

bool passingHistoricalRelease() {
    return inputContext && inputContext->historicalRelease;
}
}
