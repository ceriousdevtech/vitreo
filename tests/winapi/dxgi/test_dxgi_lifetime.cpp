// SPDX-License-Identifier: LGPL-2.1-only
// Split from the former monolithic test_dxgi.cpp; the shared fixture lives
// in dxgi_test_fixture.hpp.

#include "dxgi_test_fixture.hpp"

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

namespace vitreo::winapi::dxgi_test {
namespace {

// Source: DXGI object ownership and IUnknown AddRef/Release. Each worker owns
// its factory/adapter; Windows permits independent object lifetimes concurrently.
// Calls use the exported handlers, including the x86 argument layout.
TEST_F(DxgiManagerTest, FactoryAndAdapterLifetimesSurviveConcurrentCalls) {
    for (const bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        DxgiManager target(memory, is64, dxgi::AdapterInfo{.description = "Test GPU"});
        target.set_method_thunks(method_thunks);
        const auto exports = create_dxgi_thunks(target);
        const auto handler = [&](const char* name) -> ApiHandler {
            for (const auto& entry : exports.entries)
                if (entry.name == name) {
                    return ApiHandler{
                        [&entry](Marshaler& m) { call_export(entry, m); }};
                }
            ADD_FAILURE() << name;
            return {};
        };
        const auto create = handler("CreateDXGIFactory1");
        const auto enumerate = handler("_DXGI_EnumAdapters1");
        const auto addref = handler("_DXGI_AddRef");
        const auto release = handler("_DXGI_Release");
        constexpr usize workers = 8;
        for (usize i = 0; i < workers; ++i) {
            ASSERT_TRUE(memory.map_region(0x20000 + i * 0x4000, 0x4000,
                                          MemoryProtection::ReadWrite).is_ok());
        }
        std::barrier start(static_cast<isize>(workers));
        std::atomic<u32> failures{0};
        std::vector<std::thread> threads;
        for (usize i = 0; i < workers; ++i) {
            threads.emplace_back([&, i] {
                cpu::CpuState state;
                state.mode = is64 ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
                const GuestAddr base = 0x20000 + i * 0x4000;
                state.gpr.rsp = base + 0x3000;
                Marshaler marshaler(memory);
                marshaler.attach_cpu(state);
                const auto call = [&](const ApiHandler& fn,
                                      std::initializer_list<u64> args) {
                    usize slot = 0;
                    for (const u64 arg : args) {
                        if (is64) {
                            switch (slot) {
                            case 0: state.gpr.rcx = arg; break;
                            case 1: state.gpr.rdx = arg; break;
                            case 2: state.gpr.r8 = arg; break;
                            case 3: state.gpr.r9 = arg; break;
                            }
                        } else {
                            (void)memory.write_u32(state.gpr.rsp + 4 + slot * 4,
                                                   static_cast<u32>(arg));
                        }
                        ++slot;
                    }
                    fn(marshaler);
                    return static_cast<u32>(state.gpr.rax);
                };
                const auto pointer = [&](GuestAddr address) -> GuestAddr {
                    return is64 ? memory.read_u64(address).value()
                                : memory.read_u32(address).value();
                };
                start.arrive_and_wait();
                for (usize n = 0; n < 256; ++n) {
                    if (call(create, {IID_ADDRESS, base}) != dxgi::S_OK) {
                        ++failures; continue;
                    }
                    const GuestAddr factory = pointer(base);
                    if (call(addref, {factory}) != 2 ||
                        call(release, {factory}) != 1) ++failures;
                    if (call(enumerate, {factory, 0, base + 16}) != dxgi::S_OK) {
                        ++failures;
                    } else {
                        const GuestAddr adapter = pointer(base + 16);
                        if (!target.is_adapter_object(adapter) ||
                            !target.retain_adapter(adapter) ||
                            target.parent_factory_of(adapter) != factory) ++failures;
                        target.release_adapter(adapter);
                        if (call(release, {adapter}) != 0) ++failures;
                    }
                    if (call(release, {factory}) != 0) ++failures;
                }
            });
        }
        for (auto& thread : threads) thread.join();
        EXPECT_EQ(failures.load(), 0u);
        for (usize i = 0; i < workers; ++i)
            ASSERT_TRUE(memory.unmap_region(0x20000 + i * 0x4000, 0x4000).is_ok());
    }
}

TEST_F(DxgiManagerTest, UnlockSuspendsOnlyItsOwnGraphicsOwner) {
    DxgiManager::ApiLock first(manager);
    DxgiManager::ApiLock second(detected_manager);
    EXPECT_EQ(manager.api_lock_depth(), 1);
    EXPECT_EQ(detected_manager.api_lock_depth(), 1);
    {
        DxgiManager::ApiUnlock unlocked(manager);
        EXPECT_EQ(manager.api_lock_depth(), 0);
        EXPECT_EQ(detected_manager.api_lock_depth(), 1);
        std::thread probe([&] {
            const bool released = manager.api_mutex().try_lock();
            EXPECT_TRUE(released);
            if (released) manager.api_mutex().unlock();
            const bool unrelated = detected_manager.api_mutex().try_lock();
            EXPECT_FALSE(unrelated);
            if (unrelated) detected_manager.api_mutex().unlock();
        });
        probe.join();
    }
    EXPECT_EQ(manager.api_lock_depth(), 1);
}

}  // namespace
}  // namespace vitreo::winapi::dxgi_test
