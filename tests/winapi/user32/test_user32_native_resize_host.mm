// Native resize notifications observe AppKit; delayed delivery must not replay
// stale geometry into the live window. Guest WINDOWPOS corrections still apply.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include "user32_ext_thunk_harness.hpp"
#include <future>
#include "winapi/core/host_gui_pump.hpp"

TEST_F(User32ExtThunkTest, QueuedNativeResizeDoesNotReplayHostGeometry) {
    @autoreleasepool {
        WindowData wd{};
        wd.parent = HWND_DESKTOP;
        wd.style = WS::POPUP | WS::THICKFRAME;
        wd.client_drawn_frame = true;
        wd.title = L"Native resize feedback test";
        wd.rect = {100, 100, 420, 340};
        wd.client_rect = {0, 0, 320, 240};
        wd.wndproc = 0x401000;
        const HWND hwnd = hwnd_mgr_.create(wd);
        ASSERT_TRUE(bridge_.create_window(hwnd, *hwnd_mgr_.find(hwnd)).is_ok());
        NSWindow* window = nil;
        for (NSWindow* candidate in NSApp.windows)
            if ([candidate.title isEqualToString:@"Native resize feedback test"]) window = candidate;
        ASSERT_NE(window, nil);
        MessageSendManager send(hwnd_mgr_, queue_, bridge_);
        send.set_wndproc_callback([](HWND, u32, u64, u64, u64) -> u64 { return 0; });
        mgr_->set_message_send_manager(&send);
        // The host has advanced beyond the event the guest is about to drain.
        bridge_.set_suppress_resize_notify(true);
        [window setFrame:NSMakeRect(80, 100, 800, 600) display:NO];
        bridge_.set_suppress_resize_notify(false);
        const NSRect latest = window.frame;
        ASSERT_TRUE(bridge_.on_native_resize(hwnd, 500, 400));
        mgr_->drain_pending_native_resizes(hwnd_mgr_.find(hwnd)->owner_thread_id);
        EXPECT_TRUE(NSEqualRects(window.frame, latest));
        EXPECT_EQ(hwnd_mgr_.find(hwnd)->client_rect.width(), 500);
        // A guest modification of WINDOWPOS is a new request, not an echo.
        send.set_wndproc_callback([&](HWND, u32 msg, u64, u64 p, u64) -> u64 {
            if (msg == WM::WINDOWPOSCHANGING) {
                (void)memory_.write_u32(p + 16, 420); // x86 WINDOWPOS.cx
            }
            return 0;
        });
        ASSERT_TRUE(bridge_.on_native_resize(hwnd, 600, 450));
        mgr_->drain_pending_native_resizes(hwnd_mgr_.find(hwnd)->owner_thread_id);
        EXPECT_FALSE(NSEqualRects(window.frame, latest));
        EXPECT_EQ(hwnd_mgr_.find(hwnd)->rect.width(), 420);
        bridge_.destroy_window(hwnd);
    }
}

TEST_F(User32ExtThunkTest, GdiFlushPresentsIntermediatePixelsWithDcHeldBothBitnesses) {
    @autoreleasepool {
        WindowData wd{};
        wd.parent = HWND_DESKTOP;
        wd.style = WS::POPUP;
        wd.client_drawn_frame = true;
        wd.title = L"GDI explicit flush test";
        wd.rect = {100, 100, 164, 164};
        wd.client_rect = {0, 0, 64, 64};
        const HWND hwnd = hwnd_mgr_.create(wd);
        ASSERT_TRUE(bridge_.create_window(hwnd, *hwnd_mgr_.find(hwnd)).is_ok());
        NSWindow* window = nil;
        for (NSWindow* candidate in NSApp.windows)
            if ([candidate.title isEqualToString:@"GDI explicit flush test"]) window = candidate;
        ASSERT_NE(window, nil);
        HandleTable handles;
        GdiManager gdi(handles);
        gdi.set_hwnd_manager(&hwnd_mgr_);
        gdi.set_cocoa_bridge(&bridge_);
        const auto dc = gdi.get_dc(hwnd);
        auto exports = make_gdi32_exports(gdi);
        const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
            [](const auto& e) { return e.name == "GdiFlush"; });
        ASSERT_NE(entry, exports.entries.end());
        EXPECT_EQ(entry->stdcall_bytes, 0u);
        auto presented_red = [&]() -> int {
            for (NSView* view in window.contentView.subviews) {
                id contents = view.layer.contents;
                if (!contents || CFGetTypeID((__bridge CFTypeRef)contents) != CGImageGetTypeID()) continue;
                CGImageRef image = (__bridge CGImageRef)contents;
                CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(image));
                if (!data) continue;
                const auto offset = CGImageGetBytesPerRow(image) + 4;
                const int red = offset < static_cast<size_t>(CFDataGetLength(data))
                    ? CFDataGetBytePtr(data)[offset] : -1;
                CFRelease(data);
                return red;
            }
            return -1;
        };
        for (bool is64 : {false, true}) {
            cpu_ = cpu::CpuState{};
            if (!is64) cpu_.init_protected_mode();
            Marshaler marshaler(memory_);
            marshaler.attach_cpu(cpu_);
            ASSERT_NE(gdi.set_pixel(dc, 1, 1, 0x000000FF), 0xFFFFFFFFu);
            entry->handler(marshaler);
            EXPECT_EQ(cpu_.gpr.rax, 1u);
            EXPECT_EQ(presented_red(), 255);
            ASSERT_NE(gdi.set_pixel(dc, 1, 1, 0x00FF0000), 0xFFFFFFFFu);
            EXPECT_EQ(presented_red(), 255); // Do not expose erase-only intermediate work.
            // Explicit flush presents completed work with the DC still held.
            ASSERT_TRUE(gdi.pat_blt(dc, 1, 1, 1, 1, 0x00FF0062u));
            entry->handler(marshaler);
            EXPECT_EQ(presented_red(), 255);
            ASSERT_NE(gdi.set_pixel(dc, 1, 1, 0x00FF0000), 0xFFFFFFFFu);
            entry->handler(marshaler);
            EXPECT_EQ(presented_red(), 0);
        }
        (void)gdi.release_dc(hwnd, dc);
        bridge_.destroy_window(hwnd);
    }
}

TEST_F(User32ExtThunkTest, RetainedDcPresentationCoalescesAndCancelsOnDestruction) {
    @autoreleasepool {
        WindowData wd{};
        wd.parent = HWND_DESKTOP;
        wd.style = WS::POPUP;
        wd.client_drawn_frame = true;
        wd.title = L"Retained DC presentation test";
        wd.rect = {100, 100, 164, 164};
        wd.client_rect = {0, 0, 64, 64};
        const HWND hwnd = hwnd_mgr_.create(wd);
        ASSERT_TRUE(bridge_.create_window(hwnd, *hwnd_mgr_.find(hwnd)).is_ok());
        NSWindow* window = nil;
        for (NSWindow* candidate in NSApp.windows)
            if ([candidate.title isEqualToString:@"Retained DC presentation test"]) window = candidate;
        ASSERT_NE(window, nil);
        auto presented_red = [&]() -> int {
            for (NSView* view in window.contentView.subviews) {
                id contents = view.layer.contents;
                if (!contents || CFGetTypeID((__bridge CFTypeRef)contents) != CGImageGetTypeID()) continue;
                CGImageRef image = (__bridge CGImageRef)contents;
                CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(image));
                if (!data) continue;
                const auto offset = CGImageGetBytesPerRow(image) + 4;
                const int red = offset < static_cast<size_t>(CFDataGetLength(data))
                    ? CFDataGetBytePtr(data)[offset] : -1;
                CFRelease(data);
                return red;
            }
            return -1;
        };
        HandleTable handles;
        std::vector<std::function<void()>> ticks;
        {
            GdiManager gdi(handles);
            gdi.set_hwnd_manager(&hwnd_mgr_);
            gdi.set_cocoa_bridge(&bridge_);
            gdi.set_presentation_scheduler([&](auto callback) { ticks.push_back(std::move(callback)); });
            const auto dc = gdi.get_dc(hwnd);
            ASSERT_NE(gdi.set_pixel(dc, 1, 1, 0x000000FF), 0xFFFFFFFFu);
            ASSERT_NE(gdi.set_pixel(dc, 1, 1, 0x00FF0000), 0xFFFFFFFFu);
            ASSERT_EQ(ticks.size(), 1u); // One host tick, not one copy per primitive.
            std::promise<void> locked, release;
            auto released = release.get_future();
            std::thread worker([&] {
                GdiManager::ApiLock held(gdi);
                locked.set_value();
                released.wait();
            });
            locked.get_future().wait();
            auto tick = std::move(ticks.front());
            ticks.clear();
            tick(); // Must rearm, not block AppKit behind the worker.
            EXPECT_EQ(ticks.size(), 1u);
            release.set_value();
            worker.join();
            ASSERT_EQ(ticks.size(), 1u);
            tick = std::move(ticks.front());
            ticks.clear();
            tick();
            EXPECT_EQ(presented_red(), 0); // Latest completed pixels, DC still held.
            EXPECT_TRUE(ticks.empty()); // No continuous rendering when clean.
            const auto memory_dc = gdi.create_compatible_dc(dc);
            ASSERT_NE(gdi.set_pixel(memory_dc, 0, 0, 0), 0xFFFFFFFFu);
            EXPECT_TRUE(ticks.empty()); // A memory surface is not a window.
            ASSERT_NE(gdi.set_pixel(dc, 1, 1, 0x000000FF), 0xFFFFFFFFu);
            ASSERT_EQ(ticks.size(), 1u);
            (void)gdi.release_dc(hwnd, dc);
        }
        // Queued host work must not retain or access the destroyed manager.
        ticks.front()();
        bridge_.destroy_window(hwnd);
    }
}

TEST_F(User32ExtThunkTest, RetainedDcPresentsOnHostTickWithoutExplicitFlush) {
    @autoreleasepool {
        WindowData wd{};
        wd.parent = HWND_DESKTOP;
        wd.style = WS::POPUP;
        wd.client_drawn_frame = true;
        wd.title = L"Host presentation tick test";
        wd.rect = {100, 100, 164, 164};
        wd.client_rect = {0, 0, 64, 64};
        const HWND hwnd = hwnd_mgr_.create(wd);
        ASSERT_TRUE(bridge_.create_window(hwnd, *hwnd_mgr_.find(hwnd)).is_ok());
        NSWindow* window = nil;
        for (NSWindow* candidate in NSApp.windows)
            if ([candidate.title isEqualToString:@"Host presentation tick test"]) window = candidate;
        ASSERT_NE(window, nil);
        auto presented_red = [&]() -> int {
            for (NSView* view in window.contentView.subviews) {
                id contents = view.layer.contents;
                if (!contents || CFGetTypeID((__bridge CFTypeRef)contents) != CGImageGetTypeID()) continue;
                CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider((__bridge CGImageRef)contents));
                if (!data) continue;
                const int red = CFDataGetLength(data) ? CFDataGetBytePtr(data)[0] : -1;
                CFRelease(data);
                return red;
            }
            return -1;
        };
        HandleTable handles;
        GdiManager gdi(handles);
        gdi.set_hwnd_manager(&hwnd_mgr_);
        gdi.set_cocoa_bridge(&bridge_);
        gdi.set_presentation_scheduler(CocoaBridge::defer_presentation);
        set_host_gui_pump([&] {
            bridge_.begin_host_input_pump();
            bridge_.pump_events(queue_);
            bridge_.end_host_input_pump();
        });
        struct ClearPump { ~ClearPump() { clear_host_gui_pump(); } } clear_pump;
        const auto dc = gdi.get_dc(hwnd);
        auto exports = make_gdi32_exports(gdi);
        const auto draw = std::find_if(exports.entries.begin(), exports.entries.end(),
            [](const auto& entry) { return entry.name == "PatBlt"; });
        ASSERT_NE(draw, exports.entries.end());
        EXPECT_EQ(draw->stdcall_bytes, 24u);
        for (bool is64 : {false, true}) {
            const int expected_red = is64 ? 0 : 255;
            cpu_ = cpu::CpuState{};
            if (!is64) cpu_.init_protected_mode();
            cpu_.gpr.rsp = STACK_BASE + 0x800;
            // ROPs from MinGW wingdi.h BLACKNESS / WHITENESS.
            const u64 args[] = {dc, 0, 0, 64, 64, is64 ? 0x00000042u : 0x00FF0062u};
            if (is64) {
                cpu_.gpr.rcx = args[0]; cpu_.gpr.rdx = args[1];
                cpu_.gpr.r8 = args[2]; cpu_.gpr.r9 = args[3];
                (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, args[4]);
                (void)memory_.write_u64(cpu_.gpr.rsp + 0x30, args[5]);
            } else {
                for (u32 i = 0; i < 6; ++i)
                    (void)memory_.write_u32(cpu_.gpr.rsp + 4 + i * 4,
                                           static_cast<u32>(args[i]));
            }
            Marshaler marshaler(memory_);
            marshaler.attach_cpu(cpu_);
            draw->handler(marshaler);
            EXPECT_EQ(cpu_.gpr.rax, 1u);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (presented_red() != expected_red && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                draw->handler(marshaler);
            }
            EXPECT_EQ(presented_red(), expected_red); // No ReleaseDC, EndPaint, or GdiFlush.
        }
        (void)gdi.release_dc(hwnd, dc);
        bridge_.destroy_window(hwnd);
    }
}
