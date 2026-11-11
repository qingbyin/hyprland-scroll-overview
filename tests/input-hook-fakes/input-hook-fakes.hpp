#pragma once

// Standalone substitutes for SearchInput.cpp's external dependencies.
// std::shared_ptr has the copy/move ownership behavior relevant to this test;
// it does not reproduce Hyprland's optimizer or its cached caller cleanup.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

template <typename T> using SP = std::shared_ptr<T>;
template <typename T> using WP = std::weak_ptr<T>;

inline constexpr uint32_t WL_KEYBOARD_KEY_STATE_RELEASED = 0;
inline constexpr uint32_t WL_KEYBOARD_KEY_STATE_PRESSED = 1;
inline constexpr uint32_t WL_POINTER_BUTTON_STATE_RELEASED = 0;
inline constexpr uint32_t WL_POINTER_BUTTON_STATE_PRESSED = 1;

class IHID {
  public:
    virtual ~IHID() = default;
};

class IKeyboard : public IHID {
  public:
    struct SKeyEvent {
        uint32_t timeMs = 0;
        uint32_t keycode = 0;
        bool updateMods = false;
        uint32_t state = WL_KEYBOARD_KEY_STATE_PRESSED;
    };
};

class IPointer : public IHID {
  public:
    struct SButtonEvent {
        uint32_t timeMs = 0;
        uint32_t button = 0;
        uint32_t state = WL_POINTER_BUTTON_STATE_PRESSED;
    };
};

class CInputManager {};
using HANDLE = void*;

class CFunctionHook {
  public:
    void* m_original = nullptr;
    void* destination = nullptr;
    bool active = false;

    bool hook() {
        active = true;
        return true;
    }
};

namespace HyprlandAPI {
struct SFunctionMatch {
    std::string demangled;
    void* address = nullptr;
};
std::vector<SFunctionMatch> findFunctionsByName(HANDLE, const std::string&);
CFunctionHook* createFunctionHook(HANDLE, void* original, void* destination);
bool removeFunctionHook(HANDLE, CFunctionHook*);
}

namespace Hyprutils::Utils {
class CScopeGuard {
  public:
    explicit CScopeGuard(const std::function<void()>& fn) : m_fn(fn) {}
    ~CScopeGuard() { m_fn(); }

  private:
    std::function<void()> m_fn;
};
}

namespace Keybinds {
struct CBind {};
struct SResolvedKey {
    uint32_t code = 0;
    std::string event;
    bool operator==(const SResolvedKey&) const = default;
};
struct SPressedInput {
    SResolvedKey key;
    bool forwarded = true;
    uint32_t actionCode = 0;
    std::string submapAtPress;
    WP<IHID> device;
    std::vector<WP<CBind>> deferredBinds;
    std::vector<WP<CBind>> armedReleaseBinds;
    std::vector<WP<CBind>> suppressedBinds;
    std::vector<WP<CBind>> releaseCallbacks;
};
class CInputState {
  public:
    SPressedInput* find(const SResolvedKey& key, const SP<IHID>& device) {
        const auto found = std::ranges::find_if(m_pressed, [&](const auto& p) {
            return p.key == key && p.device.lock() == device;
        });
        return found == m_pressed.end() ? nullptr : &*found;
    }
    bool press(SPressedInput&& input) {
        m_pressed.push_back(std::move(input));
        return true;
    }
    void release(const SResolvedKey& key, const SP<IHID>& device) {
        std::erase_if(m_pressed, [&](const auto& p) {
            return p.key == key && p.device.lock() == device;
        });
    }

  private:
    std::vector<SPressedInput> m_pressed;
};
class CRegistry {
  public:
    const std::vector<SP<CBind>>& binds() const { return m_binds; }

  private:
    std::vector<SP<CBind>> m_binds;
};
class CManager {
  public:
    CInputState& inputState() { return m_state; }
    CRegistry& registry() { return m_registry; }

  private:
    CInputState m_state;
    CRegistry m_registry;
};
inline CManager* manager = nullptr;
inline CManager* mgr() { return manager; }
}
