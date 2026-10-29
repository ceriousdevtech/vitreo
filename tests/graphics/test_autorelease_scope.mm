// SPDX-License-Identifier: LGPL-2.1-only
//
// AutoreleaseScope against Apple's autorelease-pool contract (Advanced Memory
// Management Programming Guide, "Using Autorelease Pool Blocks"; NSObject
// -autorelease): an autoreleased object is released when the innermost pool
// that was current at -autorelease is drained, and not before. A thread with
// no pool of its own (every guest thread) is the case that matters.

#include "graphics/common/autorelease_scope.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>

#if __has_feature(objc_arc)
#error "test_autorelease_scope.mm sends -autorelease explicitly (MRC)"
#endif

namespace {
std::atomic<int> g_deallocs{0};
}  // namespace

@interface VitreoAutoreleaseScopeProbe : NSObject
@end

@implementation VitreoAutoreleaseScopeProbe
- (void)dealloc {
    g_deallocs.fetch_add(1, std::memory_order_relaxed);
    [super dealloc];
}
@end

namespace vitreo::graphics {
namespace {

TEST(AutoreleaseScope, DrainsOnAThreadWithNoPoolOfItsOwn) {
    g_deallocs.store(0);
    int inside = -1;
    int after = -1;
    std::thread([&] {
        {
            AutoreleaseScope scope;
            [[[VitreoAutoreleaseScopeProbe alloc] init] autorelease];
            [[[VitreoAutoreleaseScopeProbe alloc] init] autorelease];
            inside = g_deallocs.load();
        }
        after = g_deallocs.load();
    }).join();
    EXPECT_EQ(inside, 0);  // not released before the pool drains
    EXPECT_EQ(after, 2);   // released when it does
}

TEST(AutoreleaseScope, InnerScopeReleasesOnlyWhatItAutoreleased) {
    g_deallocs.store(0);
    int after_inner = -1;
    int after_outer = -1;
    std::thread([&] {
        {
            AutoreleaseScope outer;
            [[[VitreoAutoreleaseScopeProbe alloc] init] autorelease];
            {
                AutoreleaseScope inner;
                [[[VitreoAutoreleaseScopeProbe alloc] init] autorelease];
                [[[VitreoAutoreleaseScopeProbe alloc] init] autorelease];
            }
            after_inner = g_deallocs.load();
        }
        after_outer = g_deallocs.load();
    }).join();
    EXPECT_EQ(after_inner, 2);
    EXPECT_EQ(after_outer, 3);
}

TEST(AutoreleaseScope, RepeatedEntriesDoNotAccumulate) {
    // A long-lived thread making many entries: each entry's objects are gone
    // when it returns, so nothing grows with the number of calls.
    g_deallocs.store(0);
    int worst_live = 0;
    std::thread([&] {
        for (int call = 1; call <= 1000; ++call) {
            {
                AutoreleaseScope entry;
                [[[VitreoAutoreleaseScopeProbe alloc] init] autorelease];
            }
            worst_live = std::max(worst_live, call - g_deallocs.load());
        }
    }).join();
    EXPECT_EQ(g_deallocs.load(), 1000);
    EXPECT_EQ(worst_live, 0);
}

/// Growth of the device-wide MTLDevice.currentAllocatedSize across @p rounds
/// of create, write, destroy of a small CPU-writable buffer on a thread with
/// no pool, each round inside an AutoreleaseScope or not. Read before the
/// thread exits: a guest render thread never does.
u64 allocated_growth(MetalDevice& device, bool scoped, int rounds) {
    id<MTLDevice> mtl = (id<MTLDevice>)device.native_device();
    const u64 before = mtl.currentAllocatedSize;
    u64 after = before;
    std::thread([&] {
        BackendBufferDesc desc{};
        desc.byte_width = 208;
        desc.constant_buffer = true;
        desc.cpu_writable = true;
        const std::array<u8, 208> bytes{};
        for (int i = 0; i < rounds; ++i) {
            auto round = [&] {
                auto buffer = device.create_buffer(desc, bytes.data());
                ASSERT_NE(buffer, nullptr);
                buffer->update(0, bytes.data(), bytes.size(),
                               BufferWrite::Discard);
            };
            if (scoped) {
                AutoreleaseScope entry;
                round();
            } else {
                round();
            }
        }
        after = mtl.currentAllocatedSize;
    }).join();
    return after > before ? after - before : 0;
}

TEST(AutoreleaseScope, MetalBufferChurnOnAPoolLessThreadLeavesNothingAlive) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    constexpr int kRounds = 512;
    const u64 unscoped = allocated_growth(device, false, kRounds);
    const u64 scoped = allocated_growth(device, true, kRounds);
    std::fprintf(stderr, "[autorelease] %d rounds: unscoped +%llu B, scoped +%llu B\n",
                 kRounds, (unsigned long long)unscoped,
                 (unsigned long long)scoped);
    EXPECT_LT(scoped, u64{kRounds} * 4096 / 8)
        << "buffers destroyed inside a scope must not stay allocated";
}

}  // namespace
}  // namespace vitreo::graphics
