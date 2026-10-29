// SPDX-License-Identifier: LGPL-2.1-only
//
// DXGI exclusive fullscreen.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_swapchain.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// IDXGISwapChain exclusive full-screen against a window system that records
// what the shared exclusive owner (user32's virtual display mode) and the
// host Space are asked for. The output resolver reports the published mode
// as the runtime's does: DesktopCoordinates follow it, the native modes stay.
class DxgiExclusiveFullscreenTest : public DxgiSwapChainContractTest {
protected:
    static constexpr u32 kAllowModeSwitch = 0x2;  // DXGI_SWAP_CHAIN_FLAG
    static constexpr u32 kDisplayW = 2560;
    static constexpr u32 kDisplayH = 1440;
    static constexpr GuestAddr ODESC = DATA_BASE + 0x1B00;

    struct Enter {
        u64 hwnd;
        std::array<i32, 4> output;
        u32 width, height;
        bool change_mode;
        bool operator==(const Enter&) const = default;
    };
    struct Leave {
        u64 hwnd;
        bool mode_changed;
        std::optional<std::array<i32, 4>> restore;
        bool operator==(const Leave&) const = default;
    };

    void SetUp() override {
        DxgiSwapChainContractTest::SetUp();
        D3D11Manager::WindowServices window;
        window.client_size = [this](u64 hwnd)
            -> std::optional<std::pair<u32, u32>> {
            if (hwnd == kMissingWindow) return std::nullopt;
            return std::pair<u32, u32>{client_w, client_h};
        };
        window.resize_client = [this](u64 hwnd, u32 w, u32 h) {
            resized_hwnd = hwnd;
            client_w = w;
            client_h = h;
            return true;
        };
        window.window_rect = [this](u64)
            -> std::optional<std::array<i32, 4>> { return window_rect; };
        window.enter_exclusive = [this](u64 hwnd, std::array<i32, 4> output,
                                        u32 w, u32 h, bool change_mode) {
            enters.push_back({hwnd, output, w, h, change_mode});
            if (change_mode) {
                published = std::pair<u32, u32>{w, h};
            } else {
                published.reset();
            }
            client_w = w;
            client_h = h;
            window_rect = change_mode
                ? std::array<i32, 4>{0, 0, static_cast<i32>(w),
                                     static_cast<i32>(h)}
                : output;
            if (on_enter) on_enter();
            return hwnd != kMissingWindow;
        };
        window.leave_exclusive = [this](u64 hwnd, bool mode_changed,
                                        std::optional<std::array<i32, 4>> r) {
            leaves.push_back({hwnd, mode_changed, r});
            if (mode_changed) published.reset();
            if (r) window_rect = *r;
        };
        window.host_fullscreen = [this](u64 hwnd, bool enable) {
            host.emplace_back(hwnd, enable);
        };
        window.application_active = [this] { return app_active; };
        window.has_keyboard_focus = [this](u64) {
            return app_active && focused;
        };
        d3d11.set_window_services(window);
        dxgi.set_output_resolver([this] {
            dxgi::OutputInfo info{
                .device_name = "\\\\.\\DISPLAY1",
                .left = 0, .top = 0,
                .right = static_cast<i32>(kDisplayW),
                .bottom = static_cast<i32>(kDisplayH),
                .monitor = 0xD0001,
                .bits_per_color = 8,
                .refresh_rate_hz = 60.0,
            };
            if (published) {
                info.native_width = kDisplayW;
                info.native_height = kDisplayH;
                info.right = static_cast<i32>(published->first);
                info.bottom = static_cast<i32>(published->second);
            }
            return std::vector<dxgi::OutputInfo>{info};
        });
    }

    Desc chain_desc(u32 w, u32 h, u32 flags, u32 effect = 0) {
        Desc d;
        d.width = w;
        d.height = h;
        d.flags = flags;
        d.effect = effect;
        d.count = effect >= 3 ? 2 : 1;
        return d;
    }

    // GetFullscreenState(&fs, &output): {fs, output}.
    std::pair<u32, u64> fullscreen_state(GuestAddr chain) {
        EXPECT_TRUE(memory.write_u32(OUT, 7).is_ok());
        EXPECT_TRUE(memory.write_u64(OUT2, 0x1111).is_ok());
        EXPECT_EQ(call(chain, 11, {OUT, OUT2}), d3d11::S_OK);
        return {memory.read_u32(OUT).value(), memory.read_u64(OUT2).value()};
    }

    u32 output_method(GuestAddr output, usize slot,
                      std::initializer_list<u64> args) {
        set_arg(0, output);
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        dxgi.impl_OutputMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }

    std::array<i32, 4> desktop_coordinates(GuestAddr output) {
        EXPECT_EQ(output_method(output, 7, {ODESC}), d3d11::S_OK);
        return {static_cast<i32>(memory.read_u32(ODESC + 64).value()),
                static_cast<i32>(memory.read_u32(ODESC + 68).value()),
                static_cast<i32>(memory.read_u32(ODESC + 72).value()),
                static_cast<i32>(memory.read_u32(ODESC + 76).value())};
    }

    // GetDesc's Windowed (x64 DXGI_SWAP_CHAIN_DESC offset 56) and
    // GetFullscreenDesc's Windowed (offset 16).
    std::pair<u32, u32> windowed_fields(GuestAddr chain) {
        EXPECT_EQ(call(chain, 12, {SCRATCH}), d3d11::S_OK);
        const u32 desc_windowed = memory.read_u32(SCRATCH + 56).value();
        EXPECT_EQ(call(chain, 19, {SCRATCH}), d3d11::S_OK);
        return {desc_windowed, memory.read_u32(SCRATCH + 16).value()};
    }

    std::vector<Enter> enters;
    std::vector<Leave> leaves;
    std::vector<std::pair<u64, bool>> host;
    std::array<i32, 4> window_rect{300, 200, 1300, 950};
    std::optional<std::pair<u32, u32>> published;
    std::function<void()> on_enter;
    bool app_active{true};
    bool focused{true};
};


// SetFullscreenState(TRUE, NULL) with ALLOW_MODE_SWITCH and a back buffer
// that is a listed mode: Learn "Switching Modes" -- the chain switches the
// output to that mode. The output is the one under the window; the host
// presents it as a Space; GetFullscreenState reports TRUE and an output
// whose reference count it incremented; GetDesc / GetFullscreenDesc report
// Windowed FALSE; the output's DesktopCoordinates follow the mode.
TEST_F(DxgiExclusiveFullscreenTest, ModeSwitchChainEntersTheBufferMode) {
    GuestAddr chain = 0;
    ASSERT_EQ(create(chain_desc(1920, 1080, kAllowModeSwitch), &chain),
              d3d11::S_OK);
    EXPECT_EQ(fullscreen_state(chain), (std::pair<u32, u64>{0, 0}));

    EXPECT_EQ(call(chain, 10, {1, 0}), d3d11::S_OK);
    EXPECT_EQ(enters, (std::vector<Enter>{
                          {kWindow, {0, 0, 2560, 1440}, 1920, 1080, true}}));
    EXPECT_EQ(host, (std::vector<std::pair<u64, bool>>{{kWindow, true}}));
    const auto [fs, output] = fullscreen_state(chain);
    EXPECT_EQ(fs, 1u);
    ASSERT_NE(output, 0u);
    EXPECT_EQ(desktop_coordinates(output),
              (std::array<i32, 4>{0, 0, 1920, 1080}));
    // The chain holds one reference, GetFullscreenState handed out another.
    EXPECT_EQ(output_method(output, 1, {}), 3u);
    EXPECT_EQ(output_method(output, 2, {}), 2u);
    EXPECT_EQ(output_method(output, 2, {}), 1u);
    EXPECT_EQ(windowed_fields(chain), (std::pair<u32, u32>{0, 0}));
    // Repeating it changes nothing.
    EXPECT_EQ(call(chain, 10, {1, 0}), d3d11::S_OK);
    EXPECT_EQ(enters.size(), 1u);
    // GetFullscreenState's out-parameters are both optional.
    EXPECT_EQ(call(chain, 11, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(call(chain, 11, {OUT, 0}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u);

    // ResizeBuffers(0, 0, 0) takes the client area, which is now the mode;
    // the chain stays full-screen.
    EXPECT_EQ(call(chain, 13, {0, 0, 0, 0, kAllowModeSwitch}), d3d11::S_OK);
    ASSERT_EQ(call(chain, 12, {SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH).value(), 1920u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 4).value(), 1080u);
    EXPECT_EQ(fullscreen_state(chain).first, 1u);
    EXPECT_EQ(call(chain, 8, {0, 0}), d3d11::S_OK);

    // SetFullscreenState(FALSE): the Space is left, the desktop mode and
    // the window rectangle come back; the chain lets go of its output.
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(host.back(), (std::pair<u64, bool>{kWindow, false}));
    EXPECT_EQ(leaves, (std::vector<Leave>{
                          {kWindow, true,
                           std::array<i32, 4>{300, 200, 1300, 950}}}));
    EXPECT_EQ(fullscreen_state(chain), (std::pair<u32, u64>{0, 0}));
    EXPECT_EQ(windowed_fields(chain), (std::pair<u32, u32>{1, 1}));
    EXPECT_EQ(output_method(output, 2, {}), 0u) << "the chain's reference";
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK) << "already windowed";
    EXPECT_EQ(leaves.size(), 1u);
    (void)call(chain, 2, {});
}

// Without ALLOW_MODE_SWITCH "DXGI defaults to the desktop resolution for
// full-screen swap chains" (Best Practices): no mode change, the window
// covers the output; ResizeTarget keeps the desktop mode and succeeds.
TEST_F(DxgiExclusiveFullscreenTest, ChainWithoutModeSwitchKeepsTheDesktopMode) {
    GuestAddr chain = 0;
    ASSERT_EQ(create(chain_desc(1920, 1080, 0), &chain), d3d11::S_OK);
    EXPECT_EQ(call(chain, 10, {1, 0}), d3d11::S_OK);
    EXPECT_EQ(enters, (std::vector<Enter>{
                          {kWindow, {0, 0, 2560, 1440}, 2560, 1440, false}}));
    const auto [fs, output] = fullscreen_state(chain);
    EXPECT_EQ(fs, 1u);
    EXPECT_EQ(desktop_coordinates(output),
              (std::array<i32, 4>{0, 0, 2560, 1440}));
    (void)output_method(output, 2, {});
    const std::array<u32, 2> mode{1280, 720};
    ASSERT_TRUE(memory.write_bytes(SCRATCH + 0x200,
                                   reinterpret_cast<const Byte*>(mode.data()),
                                   sizeof(mode)).is_ok());
    EXPECT_EQ(call(chain, 14, {SCRATCH + 0x200}), d3d11::S_OK);
    EXPECT_EQ(enters.size(), 1u);
    EXPECT_EQ(resized_hwnd, 0u) << "a full-screen ResizeTarget moves no window";
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK);
    ASSERT_EQ(leaves.size(), 1u);
    EXPECT_FALSE(leaves[0].mode_changed);
    (void)call(chain, 2, {});
}

// ALLOW_MODE_SWITCH with a back buffer that is no listed mode: the flag's
// rule, "the display mode ... will be changed to match the dimensions of
// the application window" -- the listed mode closest to the client area.
TEST_F(DxgiExclusiveFullscreenTest, UnlistedBufferTakesTheModeNearestTheWindow) {
    GuestAddr chain = 0;
    client_w = 1270;
    client_h = 725;
    ASSERT_EQ(create(chain_desc(1000, 700, kAllowModeSwitch), &chain),
              d3d11::S_OK);
    EXPECT_EQ(call(chain, 10, {1, 0}), d3d11::S_OK);
    ASSERT_EQ(enters.size(), 1u);
    EXPECT_EQ(enters[0].width, 1280u);
    EXPECT_EQ(enters[0].height, 720u);
    EXPECT_TRUE(enters[0].change_mode);

    // ResizeTarget while full-screen "changes the display mode on the target
    // output": the closest supported mode; the current one is a no-op.
    const std::array<u32, 2> mode{1600, 905};
    ASSERT_TRUE(memory.write_bytes(SCRATCH + 0x200,
                                   reinterpret_cast<const Byte*>(mode.data()),
                                   sizeof(mode)).is_ok());
    EXPECT_EQ(call(chain, 14, {SCRATCH + 0x200}), d3d11::S_OK);
    ASSERT_EQ(enters.size(), 2u);
    EXPECT_EQ(enters[1], (Enter{kWindow, {0, 0, 2560, 1440}, 1600, 900, true}));
    EXPECT_EQ(call(chain, 14, {SCRATCH + 0x200}), d3d11::S_OK);
    EXPECT_EQ(enters.size(), 2u);
    EXPECT_EQ(fullscreen_state(chain).first, 1u);
    (void)output_method(fullscreen_state(chain).second, 2, {});
    // Back to the display's own mode: no longer a switched mode.
    const std::array<u32, 2> native{2560, 1440};
    ASSERT_TRUE(memory.write_bytes(SCRATCH + 0x200,
                                   reinterpret_cast<const Byte*>(native.data()),
                                   sizeof(native)).is_ok());
    EXPECT_EQ(call(chain, 14, {SCRATCH + 0x200}), d3d11::S_OK);
    ASSERT_EQ(enters.size(), 3u);
    EXPECT_FALSE(enters[2].change_mode);
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK);
    ASSERT_EQ(leaves.size(), 1u);
    EXPECT_FALSE(leaves[0].mode_changed);
    (void)call(chain, 2, {});
}

// Learn SetFullscreenState: DXGI_ERROR_NOT_CURRENTLY_AVAILABLE when "the
// output window does not have keyboard focus" (or is occluded); pTarget must
// be an IDXGIOutput; composition and FRAME_LATENCY_WAITABLE_OBJECT chains
// refuse the call (measured).
TEST_F(DxgiExclusiveFullscreenTest, RefusalsAreTheDocumentedErrors) {
    GuestAddr chain = 0;
    ASSERT_EQ(create(chain_desc(1920, 1080, kAllowModeSwitch), &chain),
              d3d11::S_OK);
    focused = false;
    EXPECT_EQ(call(chain, 10, {1, 0}), kNotCurrentlyAvailable);
    EXPECT_TRUE(enters.empty());
    EXPECT_EQ(fullscreen_state(chain), (std::pair<u32, u64>{0, 0}));
    focused = true;
    // A pointer that is not an output (the chain itself, the factory).
    EXPECT_EQ(call(chain, 10, {1, chain}), kIC);
    EXPECT_EQ(call(chain, 10, {1, factory}), kIC);
    EXPECT_TRUE(enters.empty());
    // An output of the adapter (GetContainingOutput's) is accepted.
    ASSERT_EQ(call(chain, 15, {OUT2}), d3d11::S_OK);
    const GuestAddr target = memory.read_u64(OUT2).value();
    ASSERT_NE(target, 0u);
    EXPECT_EQ(call(chain, 10, {1, target}), d3d11::S_OK);
    EXPECT_EQ(enters.size(), 1u);
    const auto [fs, output] = fullscreen_state(chain);
    EXPECT_EQ(fs, 1u);
    EXPECT_NE(output, target) << "full-screen creates a new output object";
    (void)output_method(output, 2, {});
    (void)output_method(target, 2, {});
    // A second chain cannot take the display mode the first one holds.
    GuestAddr second = 0;
    Desc other = chain_desc(1280, 720, kAllowModeSwitch);
    other.hwnd = 0xBEEF;
    ASSERT_EQ(create(other, &second), d3d11::S_OK);
    EXPECT_EQ(call(second, 10, {1, 0}), kNotCurrentlyAvailable);
    (void)call(second, 2, {});
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK);
    (void)call(chain, 2, {});

    // A window that does not exist is occluded.
    Desc missing = chain_desc(640, 480, kAllowModeSwitch);
    missing.hwnd = kMissingWindow;
    ASSERT_EQ(create(missing, &chain), d3d11::S_OK);
    EXPECT_EQ(call(chain, 10, {1, 0}), kNotCurrentlyAvailable);
    (void)call(chain, 2, {});
    // Composition chains refuse it outright.
    Desc comp = chain_desc(64, 48, 0, 3);
    ASSERT_EQ(create1(comp, 0, 0, true, &chain), d3d11::S_OK);
    EXPECT_EQ(call(chain, 10, {1, 0}), kIC);
    (void)call(chain, 2, {});
    // So does a FRAME_LATENCY_WAITABLE_OBJECT flip chain (TRUE and FALSE).
    Desc waitable = chain_desc(64, 48, 0x40, 4);
    waitable.hwnd = 0xF00D;
    ASSERT_EQ(create(waitable, &chain), d3d11::S_OK);
    EXPECT_EQ(call(chain, 10, {1, 0}), kIC);
    EXPECT_EQ(call(chain, 10, {0, 0}), kIC);
    (void)call(chain, 2, {});
    EXPECT_TRUE(host.size() == 2u) << "only the accepted entry and its exit";
}

// Focus loss: another application becoming foreground relinquishes
// full-screen (Learn SetFullscreenState: "DXGI may change the display state
// of a swap chain in response to end user or system requests"); the desktop
// mode and window come back, the host keeps the Space (Cmd-Tab leaves a
// Space as for any Mac full-screen app), Present reports
// DXGI_STATUS_OCCLUDED until the application is foreground again, and a
// SetFullscreenState(TRUE) then succeeds again.
TEST_F(DxgiExclusiveFullscreenTest, FocusLossRelinquishesAndReentrySucceeds) {
    GuestAddr chain = 0;
    ASSERT_EQ(create(chain_desc(1920, 1080, kAllowModeSwitch), &chain),
              d3d11::S_OK);
    ASSERT_EQ(call(chain, 10, {1, 0}), d3d11::S_OK);
    ASSERT_EQ(call(chain, 8, {0, 0}), d3d11::S_OK);

    app_active = false;
    EXPECT_EQ(call(chain, 8, {0, 0}), d3d11::DXGI_STATUS_OCCLUDED);
    EXPECT_EQ(leaves, (std::vector<Leave>{
                          {kWindow, true,
                           std::array<i32, 4>{300, 200, 1300, 950}}}));
    EXPECT_EQ(host, (std::vector<std::pair<u64, bool>>{{kWindow, true}}))
        << "the Space is kept";
    EXPECT_EQ(fullscreen_state(chain), (std::pair<u32, u64>{0, 0}));
    EXPECT_EQ(call(chain, 8, {0, 1}), d3d11::DXGI_STATUS_OCCLUDED)
        << "DXGI_PRESENT_TEST";
    EXPECT_EQ(call(chain, 10, {1, 0}), kNotCurrentlyAvailable);

    app_active = true;
    EXPECT_EQ(call(chain, 8, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(call(chain, 10, {1, 0}), d3d11::S_OK);
    ASSERT_EQ(enters.size(), 2u);
    EXPECT_EQ(enters[1].width, 1920u);
    EXPECT_EQ(fullscreen_state(chain).first, 1u);
    (void)output_method(fullscreen_state(chain).second, 2, {});
    (void)output_method(fullscreen_state(chain).second, 2, {});
    EXPECT_EQ(host.back(), (std::pair<u64, bool>{kWindow, true}));

    // A SetFullscreenState(FALSE) the application issues while it is not
    // foreground (its own fullscreen-lost reset) keeps the Space; the same
    // call once it is foreground leaves it.
    app_active = false;
    EXPECT_EQ(fullscreen_state(chain).first, 0u);
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(host.back(), (std::pair<u64, bool>{kWindow, true}));
    app_active = true;
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(host.back(), (std::pair<u64, bool>{kWindow, false}));
    (void)call(chain, 2, {});
}

// Cmd-Tab away and back (the RV There Yet black-screen report). The guest
// sees full-screen drop (GetFullscreenState FALSE, desktop mode restored,
// DXGI_STATUS_OCCLUDED while hidden), but the HOST is not touched: no Space
// exit, no host call at all, even when the guest answers with
// SetFullscreenState(FALSE) and recreates its buffers. A chain recreated
// meanwhile attaches to the same (Space) window's view. Once the host shows
// the window again Present displays -- even before the activation reaches
// the guest -- and the guest's re-entry reuses the Space.
TEST_F(DxgiExclusiveFullscreenTest, CmdTabCycleKeepsTheSpaceAndPresentsOnReturn) {
    void* const space_view = reinterpret_cast<void*>(0x5ACE);
    d3d11.set_presentation_view_resolver(
        [&](u64 hwnd) -> void* { return hwnd == kWindow ? space_view : nullptr; });
    bool occluded = false;
    D3D11Manager::WindowServices window;
    window.client_size = [this](u64) -> std::optional<std::pair<u32, u32>> {
        return std::pair<u32, u32>{client_w, client_h};
    };
    window.resize_client = [this](u64, u32 w, u32 h) {
        client_w = w;
        client_h = h;
        return true;
    };
    window.window_rect = [this](u64) -> std::optional<std::array<i32, 4>> {
        return window_rect;
    };
    window.enter_exclusive = [this](u64 hwnd, std::array<i32, 4> output, u32 w,
                                    u32 h, bool change_mode) {
        enters.push_back({hwnd, output, w, h, change_mode});
        client_w = w;
        client_h = h;
        return true;
    };
    window.leave_exclusive = [this](u64 hwnd, bool changed,
                                    std::optional<std::array<i32, 4>> r) {
        leaves.push_back({hwnd, changed, r});
    };
    window.host_fullscreen = [this](u64 hwnd, bool enable) {
        host.emplace_back(hwnd, enable);
    };
    window.application_active = [this] { return app_active; };
    window.has_keyboard_focus = [this](u64) { return app_active && focused; };
    window.window_occluded = [&](u64) { return occluded; };
    d3d11.set_window_services(window);

    // A 2560x1440 buffer on the 2560x1440 output: full-screen in the desktop
    // mode (the run that showed the black screen).
    GuestAddr chain = 0;
    ASSERT_EQ(create(chain_desc(2560, 1440, kAllowModeSwitch), &chain),
              d3d11::S_OK);
    ASSERT_EQ(call(chain, 10, {1, 0}), d3d11::S_OK);
    ASSERT_EQ(host, (std::vector<std::pair<u64, bool>>{{kWindow, true}}));
    ASSERT_EQ(call(chain, 8, {0, 0}), d3d11::S_OK);
    const unsigned presents_before = backend.state.presents;

    // Away: the user is on another app; AppKit reports the window hidden.
    app_active = false;
    occluded = true;
    EXPECT_EQ(call(chain, 8, {0, 0}), d3d11::DXGI_STATUS_OCCLUDED);
    EXPECT_EQ(backend.state.presents, presents_before);
    EXPECT_EQ(fullscreen_state(chain), (std::pair<u32, u64>{0, 0}));
    EXPECT_EQ(leaves.size(), 1u) << "guest-side desktop mode / window restore";
    // The guest's own reaction: windowed reset and new buffers. The backend
    // chain is rebuilt from its own description, whose view is the Space
    // window's (MetalSwapChain::rebuild re-creates the layer in it).
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(backend.state.last_swap_chain_view, space_view);
    const unsigned creations = backend.state.swap_chain_creations;
    const unsigned resizes = backend.state.resizes;
    EXPECT_EQ(call(chain, 13, {0, 1920, 1080, 0, kAllowModeSwitch}),
              d3d11::S_OK);
    EXPECT_EQ(backend.state.resizes, resizes + 1);
    EXPECT_EQ(backend.state.swap_chain_creations, creations)
        << "the rebuilt chain stays on the Space window's view";
    EXPECT_EQ(host, (std::vector<std::pair<u64, bool>>{{kWindow, true}}))
        << "no host Space exit, reorder or presentation change on focus loss";

    // Back: the Space slides in; the host shows the window before the
    // guest has been told it is active again.
    occluded = false;
    EXPECT_EQ(call(chain, 8, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(backend.state.presents, presents_before + 1)
        << "Present after return reaches the visible layer";
    app_active = true;
    EXPECT_EQ(call(chain, 13, {0, 2560, 1440, 0, kAllowModeSwitch}),
              d3d11::S_OK);
    EXPECT_EQ(call(chain, 10, {1, 0}), d3d11::S_OK);
    EXPECT_EQ(fullscreen_state(chain).first, 1u);
    (void)output_method(fullscreen_state(chain).second, 2, {});
    (void)output_method(fullscreen_state(chain).second, 2, {});
    for (const auto& [hwnd, enable] : host) {
        EXPECT_TRUE(enable) << "the Space never left";
    }
    EXPECT_EQ(call(chain, 8, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(backend.state.presents, presents_before + 2);
    (void)call(chain, 10, {0, 0});
    (void)call(chain, 2, {});
}

// Releasing a full-screen chain (Learn asks for SetFullscreenState(FALSE)
// first) still gives back the desktop mode, the window and the Space.
TEST_F(DxgiExclusiveFullscreenTest, ReleaseWhileFullscreenRestores) {
    GuestAddr chain = 0;
    ASSERT_EQ(create(chain_desc(1920, 1080, kAllowModeSwitch), &chain),
              d3d11::S_OK);
    ASSERT_EQ(call(chain, 10, {1, 0}), d3d11::S_OK);
    EXPECT_EQ(call(chain, 1, {}), 2u);
    EXPECT_EQ(call(chain, 2, {}), 1u);
    EXPECT_TRUE(leaves.empty());
    EXPECT_EQ(call(chain, 2, {}), 0u);
    EXPECT_EQ(host.back(), (std::pair<u64, bool>{kWindow, false}));
    ASSERT_EQ(leaves.size(), 1u);
    EXPECT_TRUE(leaves[0].mode_changed);
    EXPECT_FALSE(published.has_value());
}

// Learn CreateSwapChain: Windowed=FALSE creates the chain full-screen when
// it can; "if full-screen mode is unavailable, the swap chain will be
// created in windowed mode and DXGI_STATUS_OCCLUDED will be returned"
// (measured in a disconnected session, both CreateSwapChain and
// CreateSwapChainForHwnd with a full-screen description).
TEST_F(DxgiExclusiveFullscreenTest, WindowedFalseCreationEntersFullscreen) {
    GuestAddr chain = 0;
    Desc d = chain_desc(1920, 1080, kAllowModeSwitch);
    d.windowed = 0;
    ASSERT_EQ(create(d, &chain), d3d11::S_OK);
    EXPECT_EQ(fullscreen_state(chain).first, 1u);
    (void)output_method(fullscreen_state(chain).second, 2, {});
    (void)output_method(fullscreen_state(chain).second, 2, {});
    EXPECT_EQ(windowed_fields(chain), (std::pair<u32, u32>{0, 0}));
    EXPECT_EQ(enters.size(), 1u);
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK);
    (void)call(chain, 2, {});

    focused = false;
    ASSERT_EQ(create(d, &chain), d3d11::DXGI_STATUS_OCCLUDED);
    EXPECT_EQ(fullscreen_state(chain), (std::pair<u32, u64>{0, 0}));
    EXPECT_EQ(windowed_fields(chain), (std::pair<u32, u32>{1, 1}));
    (void)call(chain, 2, {});
    focused = true;

    // CreateSwapChainForHwnd with DXGI_SWAP_CHAIN_FULLSCREEN_DESC
    // Windowed=FALSE (x64 layout: RefreshRate, ScanlineOrdering, Scaling,
    // Windowed).
    const std::array<u32, 5> fullscreen_desc{60, 1, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(SCRATCH + 0x300,
                                   reinterpret_cast<const Byte*>(
                                       fullscreen_desc.data()),
                                   sizeof(fullscreen_desc)).is_ok());
    Desc flip = chain_desc(1920, 1080, kAllowModeSwitch, 4);
    const std::array<u32, 12> desc1{flip.width, flip.height, flip.format, 0,
                                    1, 0, flip.usage, flip.count, 0,
                                    flip.effect, 0, flip.flags};
    ASSERT_TRUE(memory.write_bytes(DESC, reinterpret_cast<const Byte*>(
                                             desc1.data()),
                                   sizeof(desc1)).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, factory);
    set_arg(1, device);
    set_arg(2, kWindow);
    set_arg(3, DESC);
    set_arg(4, SCRATCH + 0x300);
    set_arg(5, 0);
    set_arg(6, SWAP_OUT);
    d3d11.impl_CreateSwapChainForHwnd(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    chain = memory.read_u64(SWAP_OUT).value();
    EXPECT_EQ(fullscreen_state(chain).first, 1u);
    (void)output_method(fullscreen_state(chain).second, 2, {});
    (void)output_method(fullscreen_state(chain).second, 2, {});
    EXPECT_EQ(windowed_fields(chain).second, 0u);
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK);
    (void)call(chain, 2, {});
}

// Learn: "DXGI_STATUS_MODE_CHANGE_IN_PROGRESS is returned if a
// fullscreen/windowed mode transition is occurring when this API is called"
// -- here from a handler of the transition's own messages; ResizeTarget
// documents the same status.
TEST_F(DxgiExclusiveFullscreenTest, ReentryDuringTransitionIsModeChangeInProgress) {
    GuestAddr chain = 0;
    ASSERT_EQ(create(chain_desc(1920, 1080, kAllowModeSwitch), &chain),
              d3d11::S_OK);
    std::vector<u32> inner;
    on_enter = [&] {
        inner.push_back(call(chain, 10, {0, 0}));
        const std::array<u32, 2> mode{1280, 720};
        EXPECT_TRUE(memory.write_bytes(SCRATCH + 0x200,
                                       reinterpret_cast<const Byte*>(
                                           mode.data()),
                                       sizeof(mode)).is_ok());
        inner.push_back(call(chain, 14, {SCRATCH + 0x200}));
    };
    EXPECT_EQ(call(chain, 10, {1, 0}), d3d11::S_OK);
    on_enter = nullptr;
    EXPECT_EQ(inner, (std::vector<u32>{
                         d3d11::DXGI_STATUS_MODE_CHANGE_IN_PROGRESS,
                         d3d11::DXGI_STATUS_MODE_CHANGE_IN_PROGRESS}));
    EXPECT_EQ(fullscreen_state(chain).first, 1u);
    (void)output_method(fullscreen_state(chain).second, 2, {});
    (void)output_method(fullscreen_state(chain).second, 2, {});
    EXPECT_EQ(call(chain, 10, {0, 0}), d3d11::S_OK);
    (void)call(chain, 2, {});
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
