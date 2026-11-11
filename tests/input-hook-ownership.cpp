#include "SearchInput.hpp"
#include "globals.hpp"
#include <input-hook-fakes.hpp>

#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* expression, int line) {
    if (!condition)
        throw std::runtime_error("line " + std::to_string(line) + ": " +
                                 expression);
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)

using KeyboardHook = void (*)(CInputManager*, const IKeyboard::SKeyEvent&,
                             SP<IKeyboard>);
using PointerHook = void (*)(CInputManager*, IPointer::SButtonEvent,
                            SP<IPointer>);
std::vector<std::unique_ptr<CFunctionHook>> hooks;
CInputManager* expectedManager = nullptr;
size_t keyboardCalls = 0;
size_t pointerCalls = 0;
std::function<void(const IKeyboard::SKeyEvent&, const SP<IKeyboard>&)>
    keyboardObserver;
std::function<void(IPointer::SButtonEvent, const SP<IPointer>&)>
    pointerObserver;

// Four owners must coexist here: the test's external owner, the untouched
// incoming hook argument, its retained context, and the forwarded copy.
// Moving the incoming argument into this call leaves only three. Checking
// inside the actual original-call substitute catches that violation even
// though this standalone caller does not cache optimized cleanup pointers.
void keyboardOriginal(CInputManager* manager,
                      const IKeyboard::SKeyEvent& event,
                      SP<IKeyboard> device) {
    CHECK(manager == expectedManager);
    CHECK(device && device.use_count() == 4);
    CHECK(OverviewSearchInput::keyboard().get() == device.get());
    CHECK(!OverviewSearchInput::pointer());
    ++keyboardCalls;
    if (keyboardObserver)
        keyboardObserver(event, device);
}

void pointerOriginal(CInputManager* manager, IPointer::SButtonEvent event,
                     SP<IPointer> device) {
    CHECK(manager == expectedManager);
    CHECK(device && device.use_count() == 4);
    CHECK(OverviewSearchInput::pointer().get() == device.get());
    CHECK(!OverviewSearchInput::keyboard());
    ++pointerCalls;
    if (pointerObserver)
        pointerObserver(event, device);
}

void outsideContext() {
    CHECK(!OverviewSearchInput::keyboard());
    CHECK(!OverviewSearchInput::pointer());
    CHECK(!OverviewSearchInput::passingHistoricalRelease());
}

struct Keyboard : IKeyboard {
    explicit Keyboard(int& deaths) : deaths(deaths) {}
    ~Keyboard() override { ++deaths; }
    int& deaths;
};
struct Pointer : IPointer {
    explicit Pointer(int& deaths) : deaths(deaths) {}
    ~Pointer() override { ++deaths; }
    int& deaths;
};

void run() {
    CInputManager inputManager;
    expectedManager = &inputManager;
    outsideContext();
    CHECK(!OverviewSearchInput::initialize()); // no plugin handle yet
    SCROLLOVERVIEW_HANDLE = &inputManager;
    CHECK(OverviewSearchInput::initialize());
    CHECK(OverviewSearchInput::initialize()); // idempotent, no extra hooks
    CHECK(OverviewSearchInput::available());
    CHECK(hooks.size() == 2);
    CHECK(hooks[0]->active && hooks[1]->active);
    const auto keyboard = reinterpret_cast<KeyboardHook>(hooks[0]->destination);
    const auto pointer = reinterpret_cast<PointerHook>(hooks[1]->destination);

    int keyboardDeaths = 0, pointerDeaths = 0;
    SP<IKeyboard> first = std::make_shared<Keyboard>(keyboardDeaths);
    SP<IKeyboard> second = std::make_shared<Keyboard>(keyboardDeaths);
    SP<IPointer> mouse = std::make_shared<Pointer>(pointerDeaths);
    IKeyboard::SKeyEvent key{.keycode = 28};
    IPointer::SButtonEvent button{.button = 272};

    // No editor or overview is needed: both wrappers run for ordinary input.
    // Alternate devices and both states, checking balance after every call.
    for (int i = 0; i < 128; ++i) {
        key.state = uint32_t(i % 2);
        button.state = uint32_t(i % 2);
        const auto& source = i % 4 < 2 ? first : second;
        keyboardObserver = [&](const auto& event, const auto& device) {
            CHECK(event.keycode == key.keycode && event.state == key.state);
            CHECK(device.get() == source.get());
        };
        pointerObserver = [&](const auto& event, const auto& device) {
            CHECK(event.button == button.button && event.state == button.state);
            CHECK(device.get() == mouse.get());
        };
        keyboard(&inputManager, key, source);
        pointer(&inputManager, button, mouse);
        CHECK(first.use_count() == 1 && second.use_count() == 1);
        CHECK(mouse.use_count() == 1);
        CHECK(keyboardDeaths == 0 && pointerDeaths == 0);
        outsideContext();
    }
    CHECK(keyboardCalls == 128 && pointerCalls == 128);

    // Keyboard -> second keyboard -> pointer: each source remains alive and
    // the stack-local context is restored after each nested wrapper returns.
    int depth = 0;
    keyboardObserver = [&](const auto&, const auto& device) {
        if (depth == 0) {
            CHECK(device.get() == first.get());
            ++depth;
            keyboard(&inputManager, key, second);
            --depth;
            CHECK(OverviewSearchInput::keyboard().get() == first.get());
        } else {
            CHECK(device.get() == second.get());
            pointer(&inputManager, button, mouse);
            CHECK(OverviewSearchInput::keyboard().get() == second.get());
        }
        CHECK(device.use_count() == 4);
    };
    keyboard(&inputManager, key, first);
    outsideContext();

    // Actual scope-guard cleanup must also balance owners during unwinding.
    keyboardObserver = [](const auto&, const auto&) {
        throw std::runtime_error("keyboard original failure");
    };
    bool caught = false;
    try {
        keyboard(&inputManager, key, first);
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "keyboard original failure");
        caught = true;
    }
    CHECK(caught);
    outsideContext();

    pointerObserver = [](const auto&, const auto&) {
        throw std::runtime_error("pointer original failure");
    };
    caught = false;
    try {
        pointer(&inputManager, button, mouse);
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "pointer original failure");
        caught = true;
    }
    CHECK(caught);
    outsideContext();
    CHECK(first.use_count() == 1 && second.use_count() == 1);
    CHECK(mouse.use_count() == 1);
    CHECK(keyboardDeaths == 0 && pointerDeaths == 0);

    keyboardObserver = {};
    pointerObserver = {};
    first.reset();
    second.reset();
    mouse.reset();
    CHECK(keyboardDeaths == 2 && pointerDeaths == 1);
    OverviewSearchInput::shutdown();
    CHECK(!OverviewSearchInput::available());
    CHECK(hooks.empty());
    OverviewSearchInput::shutdown();
    SCROLLOVERVIEW_HANDLE = nullptr;
    outsideContext();
}
}

namespace HyprlandAPI {
std::vector<SFunctionMatch> findFunctionsByName(HANDLE,
                                               const std::string& name) {
    if (name == "onKeyboardKey")
        return {{"CInputManager::onKeyboardKey(IKeyboard::SKeyEvent const&, "
                 "Hyprutils::Memory::CSharedPointer<IKeyboard>)",
                 reinterpret_cast<void*>(keyboardOriginal)}};
    if (name == "onMouseButton")
        return {{"CInputManager::onMouseButton(IPointer::SButtonEvent, "
                 "Hyprutils::Memory::CSharedPointer<IPointer>)",
                 reinterpret_cast<void*>(pointerOriginal)}};
    return {};
}
CFunctionHook* createFunctionHook(HANDLE, void* original, void* destination) {
    auto hook = std::make_unique<CFunctionHook>();
    hook->m_original = original;
    hook->destination = destination;
    auto* result = hook.get();
    hooks.push_back(std::move(hook));
    return result;
}
bool removeFunctionHook(HANDLE, CFunctionHook* hook) {
    return std::erase_if(hooks, [hook](const auto& p) {
        return p.get() == hook;
    }) == 1;
}
}

int main() {
    try {
        run();
        std::cout << "input hook ownership: PASS (" << keyboardCalls
                  << " keyboard, " << pointerCalls << " pointer calls)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "input hook ownership: FAIL: " << error.what() << '\n';
        return 1;
    }
}
