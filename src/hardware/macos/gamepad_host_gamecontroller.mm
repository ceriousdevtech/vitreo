/// @file gamepad_host_gamecontroller.mm
/// @brief macOS GameController-framework implementation of GamepadHost.

#include "hardware/contracts/gamepad_host.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

namespace vitreo::hardware {
namespace {

constexpr u16 kDpadUp = 0x0001;
constexpr u16 kDpadDown = 0x0002;
constexpr u16 kDpadLeft = 0x0004;
constexpr u16 kDpadRight = 0x0008;
constexpr u16 kStart = 0x0010;
constexpr u16 kBack = 0x0020;
constexpr u16 kLeftThumb = 0x0040;
constexpr u16 kRightThumb = 0x0080;
constexpr u16 kLeftShoulder = 0x0100;
constexpr u16 kRightShoulder = 0x0200;
constexpr u16 kA = 0x1000;
constexpr u16 kB = 0x2000;
constexpr u16 kX = 0x4000;
constexpr u16 kY = 0x8000;

[[nodiscard]] i16 thumb_axis(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const float scaled = clamped < 0.0f ? clamped * 32768.0f
                                       : clamped * 32767.0f;
    return static_cast<i16>(std::lround(scaled));
}

[[nodiscard]] u8 trigger_axis(float value) {
    return static_cast<u8>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

void add_button(u16& buttons, GCControllerButtonInput* input, u16 mask) {
    if (input != nil && input.isPressed) buttons |= mask;
}

// GCControllerElement.h: default system gestures may consume or delay a
// mapped input (for example Options long-press capture). A Windows input host
// owns its mapped controls within this application; leave unmapped Home,
// dedicated Share/Capture and touchpad controls at their native preferences.
void configure_mapped_system_gestures(GCController* controller) {
    GCExtendedGamepad* pad = controller.extendedGamepad;
    if (pad == nil) return;
    GCControllerElement* mapped[] = {
        pad.dpad, pad.dpad.up, pad.dpad.down, pad.dpad.left, pad.dpad.right,
        pad.buttonMenu, pad.buttonOptions, pad.leftThumbstickButton,
        pad.rightThumbstickButton, pad.leftShoulder, pad.rightShoulder,
        pad.buttonA, pad.buttonB, pad.buttonX, pad.buttonY,
        pad.leftTrigger, pad.rightTrigger,
        pad.leftThumbstick, pad.leftThumbstick.xAxis, pad.leftThumbstick.yAxis,
        pad.rightThumbstick, pad.rightThumbstick.xAxis, pad.rightThumbstick.yAxis,
    };
    for (GCControllerElement* element : mapped) {
        if (element != nil)
            element.preferredSystemGestureState = GCSystemGestureStateDisabled;
    }
    const char* diag = std::getenv("VITREO_INPUT_DIAG");
    if (diag && *diag && *diag != '0') {
        const char* vendor = controller.vendorName.UTF8String;
        std::fprintf(stderr,
            "[gamepad] configured vendor=%s menu_pref=%ld options_pref=%ld\n",
            vendor ? vendor : "(unknown)",
            static_cast<long>(pad.buttonMenu.preferredSystemGestureState),
            static_cast<long>(pad.buttonOptions.preferredSystemGestureState));
    }
}

// Connection IDs are monotonically assigned while holding retained native
// identities. A reconnect notification retires the previous connection even
// when no query happened during the physical disconnect interval.
struct ControllerIdentityHash {
    usize operator()(GCController* controller) const noexcept {
        return std::hash<const void*>{}((__bridge const void*)controller);
    }
};
struct ConnectionIdentities {
    std::mutex mutex;
    std::unordered_map<GCController*,u64,ControllerIdentityHash> ids;
    u64 next{1};
    u64 identify(GCController* controller) {
        std::lock_guard lock(mutex);
        if(auto found=ids.find(controller);found!=ids.end()) return found->second;
#if !__has_feature(objc_arc)
        [controller retain];
#endif
        const auto id=next++;ids.emplace(controller,id);return id;
    }
    void retire(GCController* controller) {
        std::lock_guard lock(mutex);
        if(auto found=ids.find(controller);found!=ids.end()) {
#if !__has_feature(objc_arc)
            [found->first release];
#endif
            ids.erase(found);
        }
    }
    ~ConnectionIdentities() {
#if !__has_feature(objc_arc)
        for(const auto& entry:ids) [entry.first release];
#endif
    }
};

// GCController.h:160-169 defines this process-global, nonatomic class property.
// Serialize every capture/set/restore across host instances; no main-thread
// requirement is declared for this property. Gesture configuration stays on main.
struct BackgroundInputState {
    std::mutex mutex;
    usize consumers{};
    BOOL original{NO};
    void acquire() {
        std::lock_guard lock(mutex);
        @autoreleasepool {
            if(consumers++==0) {
                original=GCController.shouldMonitorBackgroundEvents;
                GCController.shouldMonitorBackgroundEvents=YES;
            }
        }
    }
    void release() {
        std::lock_guard lock(mutex);
        @autoreleasepool {
            if(--consumers==0) GCController.shouldMonitorBackgroundEvents=original;
        }
    }
};
struct BackgroundInputLease final : GamepadBackgroundInputLease {
    explicit BackgroundInputLease(std::shared_ptr<BackgroundInputState> state) : state(std::move(state)) {this->state->acquire();}
    ~BackgroundInputLease() override {state->release();}
    std::shared_ptr<BackgroundInputState> state;
};
std::shared_ptr<BackgroundInputState> process_background_input() {
    // The framework property belongs to the process, including multiple hosts.
    static auto state=std::make_shared<BackgroundInputState>();return state;
}

class GameControllerHost final : public GamepadHost {
public:
    GameControllerHost() {
        // Start the framework during platform-host construction, before the
        // first guest window finishes NSApplication launch. Apple's Game
        // Controller guide specifies that the initial connected-device list
        // is populated after application launch. Deferring framework activation
        // to the first XInput query can return its still-empty startup list.
        // This starts normal asynchronous enumeration; it neither waits for a
        // controller nor treats an empty snapshot as a connected device.
        // https://developer.apple.com/library/archive/documentation/ServicesDiscovery/Conceptual/GameControllerPG/DiscoveringControllers/DiscoveringControllers.html
        @autoreleasepool {
            // Subscribe before taking the initial snapshot so a concurrent
            // connection cannot fall between enumeration and observation.
            // Configure nonatomic framework properties only on the main queue.
            const auto observing = m_observing;
            const auto identities=m_identities;
            m_connection_observer = [[NSNotificationCenter defaultCenter]
                addObserverForName:GCControllerDidConnectNotification object:nil
                queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification* note) {
                    if (observing->load(std::memory_order_acquire)) {
                        GCController* controller = note.object;
                        (void)identities->identify(controller);
                        configure_mapped_system_gestures(controller);
                    }
                }];
            m_disconnection_observer=[[NSNotificationCenter defaultCenter]
                addObserverForName:GCControllerDidDisconnectNotification object:nil
                queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification* note) {
                    if(observing->load(std::memory_order_acquire)) identities->retire(note.object);
                }];
            (void)[GCController controllers];
            [[NSOperationQueue mainQueue] addOperationWithBlock:^{
                if (!observing->load(std::memory_order_acquire)) return;
                NSArray<GCController*>* controllers = [GCController controllers];
                const char* diag = std::getenv("VITREO_INPUT_DIAG");
                if (diag && *diag && *diag != '0')
                    std::fprintf(stderr, "[gamepad] initial controllers=%lu\n",
                        static_cast<unsigned long>(controllers.count));
                for (GCController* controller in controllers)
                    configure_mapped_system_gestures(controller);
            }];
        }
    }

    ~GameControllerHost() override {
        m_observing->store(false, std::memory_order_release);
        [[NSNotificationCenter defaultCenter] removeObserver:m_connection_observer];
        [[NSNotificationCenter defaultCenter] removeObserver:m_disconnection_observer];
    }

    std::shared_ptr<GamepadBackgroundInputLease> acquire_background_input() override {
        if(@available(macOS 11.3,*)) return std::make_shared<BackgroundInputLease>(process_background_input());
        return {};
    }

    [[nodiscard]] bool query_state(u32 index, GamepadState& out) override {
        @autoreleasepool {
            out = GamepadState{};
            NSArray<GCController*>* controllers = [GCController controllers];
            if (index >= controllers.count) return true;
            GCExtendedGamepad* pad = controllers[index].extendedGamepad;
            if (pad == nil) return true;

            out.connected = true;
            out.connection_id=m_identities->identify(controllers[index]);
            add_button(out.buttons, pad.dpad.up, kDpadUp);
            add_button(out.buttons, pad.dpad.down, kDpadDown);
            add_button(out.buttons, pad.dpad.left, kDpadLeft);
            add_button(out.buttons, pad.dpad.right, kDpadRight);
            add_button(out.buttons, pad.buttonMenu, kStart);
            add_button(out.buttons, pad.buttonOptions, kBack);
            add_button(out.buttons, pad.leftThumbstickButton, kLeftThumb);
            add_button(out.buttons, pad.rightThumbstickButton, kRightThumb);
            add_button(out.buttons, pad.leftShoulder, kLeftShoulder);
            add_button(out.buttons, pad.rightShoulder, kRightShoulder);
            add_button(out.buttons, pad.buttonA, kA);
            add_button(out.buttons, pad.buttonB, kB);
            add_button(out.buttons, pad.buttonX, kX);
            add_button(out.buttons, pad.buttonY, kY);

            out.left_trigger = trigger_axis(pad.leftTrigger.value);
            out.right_trigger = trigger_axis(pad.rightTrigger.value);
            out.left_x = thumb_axis(pad.leftThumbstick.xAxis.value);
            out.left_y = thumb_axis(pad.leftThumbstick.yAxis.value);
            out.right_x = thumb_axis(pad.rightThumbstick.xAxis.value);
            out.right_y = thumb_axis(pad.rightThumbstick.yAxis.value);
            return true;
        }
    }

private:
    std::shared_ptr<std::atomic<bool>> m_observing =
        std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<ConnectionIdentities> m_identities=std::make_shared<ConnectionIdentities>();
    id m_connection_observer{nil};
    id m_disconnection_observer{nil};
};

} // namespace

std::shared_ptr<GamepadHost> make_platform_gamepad_host() {
    return std::make_shared<GameControllerHost>();
}

} // namespace vitreo::hardware
