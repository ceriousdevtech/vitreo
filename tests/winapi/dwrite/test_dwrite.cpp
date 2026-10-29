// SPDX-License-Identifier: LGPL-2.1-only

#include "dwrite_manager_harness.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::dwrite_test;

TEST_F(DWriteManagerTest, CreatesNativeWidthFactoryWithValidVtable) {
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    const GuestAddr vtable = memory.read_u64(factory).value();
    EXPECT_EQ(memory.read_u64(vtable).value(), QUERY_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 8).value(), ADDREF_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 16).value(), RELEASE_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 24).value(), GET_COLLECTION_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 10 * 8).value(),
              CREATE_RENDERING_PARAMS_THUNK);
}

TEST_F(DWriteManagerTest, SerializesConcurrentSharedFactoryObjectCreation) {
    auto exports = create_dwrite_thunks(manager);
    const auto create_factory = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "DWriteCreateFactory";
        });
    ASSERT_NE(create_factory, exports.entries.end());

    constexpr usize kThreadCount = 8;
    constexpr usize kCallsPerThread = 32;
    std::atomic<usize> ready{0};
    std::atomic<bool> start{false};
    std::atomic<usize> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (usize index = 0; index < kThreadCount; ++index) {
        threads.emplace_back([&, index] {
            cpu::CpuState thread_cpu{};
            thread_cpu.mode = cpu::CpuMode::Long;
            Marshaler thread_marshaler(memory);
            thread_marshaler.attach_cpu(thread_cpu);
            const GuestAddr output = DATA_BASE + 0x300 + index * 8;
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (usize call = 0; call < kCallsPerThread; ++call) {
                thread_cpu.gpr.rcx = 0;
                thread_cpu.gpr.rdx = IID_ADDRESS;
                thread_cpu.gpr.r8 = output;
                create_factory->handler(thread_marshaler);
                if (thread_cpu.gpr.rax != 0 ||
                    memory.read_u64(output).value_or(0) == 0) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != kThreadCount) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(failures.load(), 0u);
}

TEST_F(DWriteManagerTest, IncompleteThunkTableCannotCreateFactoryOnRetry) {
    DWriteManager incomplete_manager{memory, true};
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    incomplete_manager.impl_DWriteCreateFactory(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x88985000u);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);

    incomplete_manager.impl_DWriteCreateFactory(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x88985000u);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);

    incomplete_manager.set_method_thunks(method_thunks());
    incomplete_manager.impl_DWriteCreateFactory(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_NE(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);
}

TEST_F(DWriteManagerTest, UnsupportedFactoryMethodsUseSignatureCompatibleThunks) {
    const GuestAddr factory = create_factory();
    const GuestAddr vtable = memory.read_u64(factory).value();
    const std::array<u32, 36> expected_arg_counts{
        5, 2, 2, 4, 5, 7, 2, 3, 7, 2,
        2, 9, 2, 2, 7, 10, 3, 2, 5, 9,
        // IDWriteFactory1, IDWriteFactory2, IDWriteFactory3.
        3, 8, 2, 2, 9, 9, 10, 10, 9, 6,
        5, 2, 2, 3, 4, 2,
    };
    for (u32 i = 0; i < expected_arg_counts.size(); ++i) {
        const u32 slot = i + 4;
        GuestAddr expected =
            NOTIMPL_THUNK_BASE + expected_arg_counts[i] * 0x10;
        switch (slot) {
        case 4: expected = CREATE_CUSTOM_COLLECTION_THUNK; break;
        case 5: expected = REGISTER_COLLECTION_LOADER_THUNK; break;
        case 6: expected = UNREGISTER_COLLECTION_LOADER_THUNK; break;
        case 7: expected = CREATE_FONT_FILE_REFERENCE_THUNK; break;
        case 8: expected = CREATE_CUSTOM_FONT_FILE_REFERENCE_THUNK; break;
        case 10: expected = CREATE_RENDERING_PARAMS_THUNK; break;
        case 11: expected = CREATE_MONITOR_RENDERING_PARAMS_THUNK; break;
        case 13: expected = REGISTER_FILE_LOADER_THUNK; break;
        case 14: expected = UNREGISTER_FILE_LOADER_THUNK; break;
        case 16: expected = CREATE_TYPOGRAPHY_THUNK; break;
        case 17: expected = GET_GDI_INTEROP_THUNK; break;
        case 21: expected = CREATE_TEXT_ANALYZER_THUNK; break;
        case 22: expected = CREATE_NUMBER_SUBSTITUTION_THUNK; break;
        case 23: expected = CREATE_GLYPH_RUN_ANALYSIS_THUNK; break;
        case 26: expected = GET_FALLBACK_THUNK; break;
        case 27: expected = CREATE_FALLBACK_BUILDER_THUNK; break;
        case 30: expected = CREATE_GLYPH_RUN_ANALYSIS2_THUNK; break;
        case 35: expected = GET_SYSTEM_FONT_SET_THUNK; break;
        case 39: expected = GET_FONT_DOWNLOAD_QUEUE_THUNK; break;
        default: break;
        }
        EXPECT_EQ(memory.read_u64(vtable + slot * 8).value(), expected)
            << "factory vtable slot " << slot;
    }
}

TEST_F(DWriteManagerTest, UnsupportedThunksDeclareExactX86StackCleanup) {
    const auto exports = create_dwrite_thunks(manager);
    const auto find = [&exports](const std::string& name) -> const ExportEntry* {
        const auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        return entry == exports.entries.end() ? nullptr : &*entry;
    };
    for (const u32 arg_count : DWRITE_NOT_IMPL_ARG_COUNTS) {
        const auto* entry =
            find("_DWrite_NotImpl" + std::to_string(arg_count));
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->stdcall_bytes, static_cast<u16>(arg_count * 4));
    }
}

TEST_F(DWriteManagerTest, QueryInterfacePreservesIdentityAndReferenceCount) {
    const GuestAddr factory = create_factory();
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
    Marshaler query(memory);
    query.attach_cpu(cpu);
    manager.impl_QueryInterface(query);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(QUERY_OUTPUT_ADDRESS).value(), factory);

    cpu.gpr.rcx = factory;
    Marshaler release(memory);
    release.attach_cpu(cpu);
    manager.impl_Release(release);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    manager.impl_Release(release);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(DWriteManagerTest, Factory3QueryPreservesIdentityAndHasCompleteVtable) {
    const GuestAddr factory = create_factory();
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_IDWRITE_FACTORY3),
        sizeof(IID_IDWRITE_FACTORY3)).is_ok());

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
    Marshaler query(memory);
    query.attach_cpu(cpu);
    manager.impl_QueryInterface(query);

    ASSERT_EQ(cpu.gpr.rax, 0u);
    ASSERT_EQ(memory.read_u64(QUERY_OUTPUT_ADDRESS).value(), factory);
    const GuestAddr vtable = memory.read_u64(factory).value();
    ASSERT_NE(vtable, 0u);
    for (u32 slot = 0; slot < 40; ++slot) {
        EXPECT_NE(memory.read_u64(vtable + slot * 8).value(), 0u)
            << "IDWriteFactory3 vtable slot " << slot;
    }
    EXPECT_EQ(memory.read_u64(vtable + 3 * 8).value(), GET_COLLECTION_THUNK);

    cpu.gpr.rcx = factory;
    Marshaler release(memory);
    release.attach_cpu(cpu);
    manager.impl_Release(release);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

// COM negotiation rules: unsupported IIDs must fail closed with a nulled out
// parameter. FontFace1–7 are implemented; probe with a nonsense IID instead.
TEST_F(DWriteManagerTest, UnsupportedIidReturnsNoInterfaceAndNullsOutParam) {
    const GuestAddr factory = create_factory();

    const Guid unsupported{
        0xDEADBEEFu, 0xDEADu, 0xBEEFu,
        {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&unsupported),
        sizeof(unsupported)).is_ok());
    ASSERT_TRUE(memory.write_u64(QUERY_OUTPUT_ADDRESS, 0xDEADBEEFu).is_ok());

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
    Marshaler query(memory);
    query.attach_cpu(cpu);
    manager.impl_QueryInterface(query);

    constexpr u32 E_NOINTERFACE_HR = 0x80004002;
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), E_NOINTERFACE_HR);
    EXPECT_EQ(memory.read_u64(QUERY_OUTPUT_ADDRESS).value(), 0u)
        << "a failed QueryInterface must null the out parameter";
}

TEST_F(DWriteManagerTest, QueryInterfaceRejectsNullOutParameter) {
    const GuestAddr factory = create_factory();
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = 0;
    Marshaler query(memory);
    query.attach_cpu(cpu);
    manager.impl_QueryInterface(query);
    constexpr u32 E_POINTER_HR = 0x80004003;
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), E_POINTER_HR);
}

// IUnknown must be answerable on every object, and the returned pointer has to
// carry that object's own fully populated vtable.
TEST_F(DWriteManagerTest, IUnknownQuerySucceedsWithFullyPopulatedVtable) {
    const GuestAddr factory = create_factory();
    const Guid iid_unknown{
        0x00000000u, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&iid_unknown),
        sizeof(iid_unknown)).is_ok());

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
    Marshaler query(memory);
    query.attach_cpu(cpu);
    manager.impl_QueryInterface(query);

    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr returned =
        static_cast<GuestAddr>(memory.read_u64(QUERY_OUTPUT_ADDRESS).value());
    ASSERT_NE(returned, 0u) << "S_OK must never come with a null interface";
    EXPECT_EQ(returned, factory);

    // Every slot the factory interface declares must be non-null.
    const GuestAddr vtable =
        static_cast<GuestAddr>(memory.read_u64(returned).value());
    ASSERT_NE(vtable, 0u);
    for (u32 slot = 0; slot < 40; ++slot) {
        const u64 entry = memory.read_u64(vtable + slot * 8).value();
        EXPECT_NE(entry, 0u) << "IDWriteFactory3 vtable slot " << slot
                             << " is null; calling it would reach EIP 0";
    }
}

// AddRef/Release must balance: an early free would leave the guest calling
// through a recycled object.
TEST_F(DWriteManagerTest, AddRefAndReleaseBalance) {
    const GuestAddr factory = create_factory();
    cpu.gpr.rcx = factory;
    Marshaler ref(memory);
    ref.attach_cpu(cpu);

    manager.impl_AddRef(ref);
    EXPECT_EQ(cpu.gpr.rax, 2u);
    manager.impl_AddRef(ref);
    EXPECT_EQ(cpu.gpr.rax, 3u);
    manager.impl_Release(ref);
    EXPECT_EQ(cpu.gpr.rax, 2u);
    manager.impl_Release(ref);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    manager.impl_Release(ref);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    // Gone: a further call must not resurrect it.
    manager.impl_AddRef(ref);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(DWriteManagerTest, ReleasedObjectsDoNotExhaustGuestComArena) {
    Marshaler call(memory);
    call.attach_cpu(cpu);

    // The backing arena has fewer than 2,000 object slots. Repeatedly creating
    // and releasing factories must recycle dead COM objects rather than make a
    // later DWriteCreateFactory fail because of allocator bookkeeping.
    for (u32 i = 0; i < 2048; ++i) {
        cpu.gpr.rcx = 0;
        cpu.gpr.rdx = IID_ADDRESS;
        cpu.gpr.r8 = OUTPUT_ADDRESS;
        manager.impl_DWriteCreateFactory(call);
        ASSERT_EQ(cpu.gpr.rax, 0u) << "iteration " << i;
        const GuestAddr factory =
            memory.read_u64(OUTPUT_ADDRESS).value_or(0);
        ASSERT_NE(factory, 0u) << "iteration " << i;

        cpu.gpr.rcx = factory;
        manager.impl_Release(call);
        ASSERT_EQ(cpu.gpr.rax, 0u) << "iteration " << i;
    }
}

TEST_F(DWriteManagerTest, ReleasedSystemCollectionCanBeRecreated) {
    const GuestAddr factory = create_factory();
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    cpu.gpr.r8 = 0;
    Marshaler get_collection(memory);
    get_collection.attach_cpu(cpu);
    manager.impl_GetSystemFontCollection(get_collection);
    const GuestAddr first = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();

    cpu.gpr.rcx = first;
    Marshaler release(memory);
    release.attach_cpu(cpu);
    manager.impl_Release(release);
    ASSERT_EQ(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = factory;
    manager.impl_GetSystemFontCollection(get_collection);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    // Once Release returns zero the old interface pointer is invalid; COM does
    // not require a later object allocation to use a different address.
    EXPECT_NE(memory.read_u64(QUERY_OUTPUT_ADDRESS).value(), 0u);
}
