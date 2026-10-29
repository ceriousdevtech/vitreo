/// @file test_thunk.cpp
/// @brief Unit tests for ThunkRegistry and Marshaler

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>

#include <atomic>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// ThunkRegistry Tests
// =============================================================================

class ThunkRegistryTest : public ::testing::Test {
protected:
    static constexpr GuestAddr THUNK_BASE = 0x70000000;
    
    ThunkRegistry registry{THUNK_BASE};
};

TEST_F(ThunkRegistryTest, InitialState) {
    EXPECT_EQ(registry.dll_count(), 0u);
    EXPECT_EQ(registry.function_count(), 0u);
    EXPECT_EQ(registry.thunk_base(), THUNK_BASE);
    EXPECT_EQ(registry.next_thunk_address(), THUNK_BASE);
}

// A missing-export registration (any guest thread resolving an import at
// LoadLibrary time) appends to thunks_ and addr_to_thunk_ while every other
// thread's dispatcher resolves handlers and stdcall sizes by slab index.
// Readers took no lock on the slab path, so a deque block-map growth under
// a concurrent thunks_[idx] was a torn read. 4 writers x 2000 registrations
// and 4 readers over every address published so far.
TEST_F(ThunkRegistryTest, TablesSurviveConcurrentMissingExportAndResolve) {
    DllExports exports;
    for (int i = 0; i < 64; ++i) {
        exports.entries.push_back(ExportEntry{
            "Fn" + std::to_string(i), static_cast<u16>(i + 1),
            [](Marshaler&) {}, static_cast<u16>(4 * (i % 4))});
    }
    ASSERT_TRUE(registry.register_dll("kernel32.dll", std::move(exports)).is_ok());
    constexpr int kWriters = 4;
    constexpr int kReaders = 4;
    constexpr int kRounds = 2000;
    std::atomic<int> failures{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < kWriters; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kRounds; ++i) {
                const std::string name = "Missing_" + std::to_string(t) + "_" +
                                         std::to_string(i);
                const GuestAddr stub = registry.register_missing_export(
                    "absent.dll", name, 0);
                if (stub == 0) ++failures;
                // Dedup contract: the same import resolves to the same stub.
                if (registry.register_missing_export("absent.dll", name, 0) != stub) {
                    ++failures;
                }
                if (registry.resolve_name(stub) != "absent.dll!" + name) ++failures;
            }
        });
    }
    for (int t = 0; t < kReaders; ++t) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const GuestAddr end = registry.next_thunk_address();
                for (GuestAddr a = THUNK_BASE; a < end; a += 16) {
                    if (registry.resolve_handler(a).is_err()) ++failures;
                    (void)registry.resolve_stdcall_bytes(a);
                    if (registry.resolve_name(a).empty()) ++failures;
                    if (registry.resolve_address(a).is_err()) ++failures;
                }
                if (registry.get_thunk_address("kernel32.dll", "Fn7").is_err()) ++failures;
                if (registry.get_thunk_address("kernel32.dll", u16{8}).is_err()) ++failures;
                if (!registry.has_dll("kernel32.dll")) ++failures;
                (void)registry.enumerate_exports();
            }
        });
    }
    for (int t = 0; t < kWriters; ++t) workers[static_cast<usize>(t)].join();
    stop = true;
    for (int t = kWriters; t < kWriters + kReaders; ++t) workers[static_cast<usize>(t)].join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(registry.function_count(), 64u);
    EXPECT_EQ(registry.missing_export_call_count("absent.dll", "Missing_0_0"), 0u);
}

TEST(ThunkRegistryAddressLimitTest,
     Linear64ThunkSlabAndBackingMapRemainBelowGuestCeiling) {
    GuestMemory memory;
    constexpr GuestAddr kLinear64Ceiling = 0x0000007F'FFFFFFFFULL;
    constexpr GuestAddr kThunkHeadroom = 0x00000005'00000000ULL;
    memory.set_maximum_user_address(kLinear64Ceiling);

    const GuestAddr thunk_base =
        ((memory.maximum_user_address() + 1) - kThunkHeadroom) &
        ~GuestAddr{0xFFFF};
    EXPECT_EQ(thunk_base, 0x0000007B'00000000ULL);
    ThunkRegistry limited_registry{thunk_base};

    DllExports exports;
    exports.entries.push_back(
        ExportEntry{"Linear64Probe", 1, [](Marshaler&) {}});
    ASSERT_TRUE(
        limited_registry.register_dll("probe.dll", std::move(exports)).is_ok());

    ASSERT_LT(limited_registry.thunk_base(), kLinear64Ceiling + 1);
    ASSERT_LE(limited_registry.thunk_end(), kLinear64Ceiling + 1);
    const usize mapped_size = static_cast<usize>(
        ((limited_registry.thunk_end() - limited_registry.thunk_base()) +
         0xFFF) & ~GuestAddr{0xFFF});
    ASSERT_NE(mapped_size, 0u);
    ASSERT_TRUE(memory.map_region(limited_registry.thunk_base(), mapped_size,
                                  MemoryProtection::ReadExecute).is_ok());
    EXPECT_TRUE(memory.is_mapped(limited_registry.thunk_base(), mapped_size));
    EXPECT_LE(limited_registry.thunk_base() + mapped_size,
              kLinear64Ceiling + 1);
}

TEST_F(ThunkRegistryTest, RegisterEmptyDll) {
    DllExports exports;
    auto result = registry.register_dll("empty.dll", std::move(exports));
    
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(registry.dll_count(), 1u);
    EXPECT_EQ(registry.function_count(), 0u);
    EXPECT_TRUE(registry.has_dll("empty.dll"));
}

TEST_F(ThunkRegistryTest, RegisterDllWithFunctions) {
    bool handler1_called = false;
    bool handler2_called = false;
    
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "GetLastError", 1,
        [&](Marshaler&) { handler1_called = true; }
    });
    exports.entries.push_back(ExportEntry{
        "SetLastError", 2,
        [&](Marshaler&) { handler2_called = true; }
    });
    
    auto result = registry.register_dll("kernel32.dll", std::move(exports));
    
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(registry.dll_count(), 1u);
    EXPECT_EQ(registry.function_count(), 2u);
}

TEST_F(ThunkRegistryTest, DuplicateDllFails) {
    DllExports exports1;
    auto result1 = registry.register_dll("test.dll", std::move(exports1));
    EXPECT_TRUE(result1.is_ok());
    
    DllExports exports2;
    auto result2 = registry.register_dll("test.dll", std::move(exports2));
    EXPECT_TRUE(result2.is_err());
    EXPECT_EQ(result2.error(), Error::AlreadyExists);
}

TEST_F(ThunkRegistryTest, DllNameCaseInsensitive) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{"TestFunc", 1, [](Marshaler&) {}});
    
    auto result = registry.register_dll("KERNEL32.DLL", std::move(exports));
    EXPECT_TRUE(result.is_ok());
    
    EXPECT_TRUE(registry.has_dll("kernel32.dll"));
    EXPECT_TRUE(registry.has_dll("KERNEL32.DLL"));
    EXPECT_TRUE(registry.has_dll("Kernel32.Dll"));
}

TEST_F(ThunkRegistryTest, GetThunkAddressByName) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{"GetLastError", 1, [](Marshaler&) {}});
    exports.entries.push_back(ExportEntry{"SetLastError", 2, [](Marshaler&) {}});
    
    registry.register_dll("kernel32.dll", std::move(exports));
    
    auto addr1 = registry.get_thunk_address("kernel32.dll", "GetLastError");
    EXPECT_TRUE(addr1.is_ok());
    EXPECT_EQ(addr1.value(), THUNK_BASE);
    
    auto addr2 = registry.get_thunk_address("kernel32.dll", "SetLastError");
    EXPECT_TRUE(addr2.is_ok());
    EXPECT_EQ(addr2.value(), THUNK_BASE + 16);  // Default spacing
}

TEST_F(ThunkRegistryTest, GetThunkAddressByOrdinal) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{"", 100, [](Marshaler&) {}});  // Ordinal-only
    exports.entries.push_back(ExportEntry{"NamedFunc", 101, [](Marshaler&) {}});
    
    registry.register_dll("test.dll", std::move(exports));
    
    auto addr1 = registry.get_thunk_address("test.dll", static_cast<u16>(100));
    EXPECT_TRUE(addr1.is_ok());
    
    auto addr2 = registry.get_thunk_address("test.dll", static_cast<u16>(101));
    EXPECT_TRUE(addr2.is_ok());
    
    // Different ordinals should have different addresses
    EXPECT_NE(addr1.value(), addr2.value());
}

TEST_F(ThunkRegistryTest, GetThunkAddressNotFoundDll) {
    auto result = registry.get_thunk_address("nonexistent.dll", "SomeFunc");
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::NotFound);
}

TEST_F(ThunkRegistryTest, GetThunkAddressNotFoundFunction) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{"ExistingFunc", 1, [](Marshaler&) {}});
    registry.register_dll("test.dll", std::move(exports));
    
    auto result = registry.get_thunk_address("test.dll", "NonExistentFunc");
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::NotFound);
}

TEST_F(ThunkRegistryTest, ResolveAddress) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{"TestFunc", 1, [](Marshaler&) {}});
    registry.register_dll("test.dll", std::move(exports));
    
    auto addr_result = registry.get_thunk_address("test.dll", "TestFunc");
    ASSERT_TRUE(addr_result.is_ok());
    
    auto api_result = registry.resolve_address(addr_result.value());
    EXPECT_TRUE(api_result.is_ok());
    EXPECT_EQ(api_result.value().dll_id, 0u);
    EXPECT_EQ(api_result.value().function_id, 0u);
}

TEST_F(ThunkRegistryTest, EntryStubAndDispatchGatewayShareHandler) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{"TestFunc", 1, [](Marshaler&) {}});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());

    auto entry = registry.get_thunk_address("test.dll", "TestFunc");
    ASSERT_TRUE(entry.is_ok());
    EXPECT_TRUE(registry.is_stub_address(entry.value()));
    EXPECT_TRUE(registry.is_stub_address(entry.value() + ThunkRegistry::STUB_SIZE - 1));
    EXPECT_FALSE(registry.is_dispatch_address(entry.value()));

    GuestAddr gateway = entry.value() + ThunkRegistry::STUB_SIZE;
    EXPECT_TRUE(registry.is_dispatch_address(gateway));
    EXPECT_FALSE(registry.is_stub_address(gateway));
    EXPECT_TRUE(registry.resolve_handler(gateway).is_ok());
    EXPECT_EQ(registry.resolve_name(entry.value() + 1), "test.dll!TestFunc");
    EXPECT_EQ(registry.resolve_name(gateway), "test.dll!TestFunc");
}

// Contiguous slab: dispatch gateway resolves via index math
// (idx = (addr - base) / spacing), not the addr_to_thunk_ hash map.
TEST_F(ThunkRegistryTest, ResolveHandlerViaDispatchGatewaySlabIndex) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{"Alpha", 1, [](Marshaler&) {}, 4});
    exports.entries.push_back(ExportEntry{"Beta", 2, [](Marshaler&) {}, 8});
    exports.entries.push_back(ExportEntry{"Gamma", 3, [](Marshaler&) {}, 12});
    ASSERT_TRUE(registry.register_dll("slab.dll", std::move(exports)).is_ok());

    const char* names[] = {"Alpha", "Beta", "Gamma"};
    const u16 stdcall_bytes[] = {4, 8, 12};
    for (usize i = 0; i < 3; ++i) {
        auto stub = registry.get_thunk_address("slab.dll", names[i]);
        ASSERT_TRUE(stub.is_ok());
        EXPECT_EQ(stub.value(), THUNK_BASE + i * 16u);

        const GuestAddr dispatch = stub.value() + ThunkRegistry::STUB_SIZE;
        EXPECT_TRUE(registry.is_dispatch_address(dispatch));
        EXPECT_EQ((dispatch - THUNK_BASE) % 16u, ThunkRegistry::STUB_SIZE);
        EXPECT_EQ((dispatch - THUNK_BASE) / 16u, i);

        auto stub_handler = registry.resolve_handler(stub.value());
        auto dispatch_handler = registry.resolve_handler(dispatch);
        ASSERT_TRUE(stub_handler.is_ok());
        ASSERT_TRUE(dispatch_handler.is_ok());
        EXPECT_EQ(stub_handler.value(), dispatch_handler.value());
        EXPECT_EQ(registry.resolve_stdcall_bytes(dispatch), stdcall_bytes[i]);
        EXPECT_EQ(registry.resolve_name(dispatch),
                  std::string("slab.dll!") + names[i]);
    }

    // Inter-stub padding is not a stub or gateway — map miss, not slab hit.
    EXPECT_TRUE(registry.resolve_handler(THUNK_BASE + ThunkRegistry::STUB_SIZE + 1)
                    .is_err());
}

TEST_F(ThunkRegistryTest, ExternalDispatchAliasSharesHandlerExactly) {
    bool handler_called = false;
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "NtTest", 7,
        [&](Marshaler&) { handler_called = true; }});
    ASSERT_TRUE(registry.register_dll("ntdll.dll", std::move(exports)).is_ok());

    const GuestAddr thunk =
        registry.get_thunk_address("ntdll.dll", "NtTest").value();
    constexpr GuestAddr alias = 0x00007FFC'00001238ULL;
    ASSERT_TRUE(registry.register_dispatch_alias(alias, thunk));
    EXPECT_TRUE(registry.register_dispatch_alias(alias, thunk));
    EXPECT_TRUE(registry.is_dispatch_address(alias));
    EXPECT_FALSE(registry.is_dispatch_address(alias + 1));
    EXPECT_EQ(registry.resolve_name(alias), "ntdll.dll!NtTest");

    // Ordinary guest code addresses must stay false without relying on the
    // alias map hitting — this is the run-loop hot path.
    EXPECT_FALSE(registry.is_dispatch_address(0x00401000));
    EXPECT_FALSE(registry.is_dispatch_address(THUNK_BASE + 1));

    auto handler = registry.resolve_handler(alias);
    ASSERT_TRUE(handler.is_ok());
    GuestMemory memory;
    Marshaler marshaler(memory);
    (*handler.value())(marshaler);
    EXPECT_TRUE(handler_called);

    EXPECT_FALSE(registry.register_dispatch_alias(
        thunk + 1, thunk));
    EXPECT_FALSE(registry.register_dispatch_alias(
        alias + 1, 0xDEADBEEF));
}

TEST_F(ThunkRegistryTest, ResolveHandler) {
    bool handler_called = false;
    
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "TestFunc", 1,
        [&](Marshaler&) { handler_called = true; }
    });
    registry.register_dll("test.dll", std::move(exports));
    
    auto addr_result = registry.get_thunk_address("test.dll", "TestFunc");
    ASSERT_TRUE(addr_result.is_ok());
    
    auto handler_result = registry.resolve_handler(addr_result.value());
    EXPECT_TRUE(handler_result.is_ok());
    
    // Create memory and marshaler for handler invocation
    GuestMemory memory;
    memory.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite);
    Marshaler marshaler(memory);
    
    (*handler_result.value())(marshaler);
    EXPECT_TRUE(handler_called);
}

// resolve_handler / resolve_stdcall_bytes answer from a per-thread cache. A
// registry built where a destroyed one lived (every test fixture does this)
// must answer with ITS handlers: a cache keyed by the registry's address
// returned the dead registry's freed std::function. Placement-new forces the
// same address; 128 thunks resolved round-robin exercise every cache entry.
TEST(ThunkRegistryResolveCacheTest, RegistryAtAReusedAddressResolvesItsOwnHandlers) {
    constexpr GuestAddr kBase = 0x70000000;
    constexpr int kFunctions = 128;
    alignas(ThunkRegistry) unsigned char storage[sizeof(ThunkRegistry)];
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite)
                    .is_ok());
    Marshaler marshaler(memory);
    for (int generation = 0; generation < 3; ++generation) {
        auto* registry = new (storage) ThunkRegistry(kBase);
        int called = -1;
        DllExports exports;
        for (int i = 0; i < kFunctions; ++i) {
            const int id = generation * 1000 + i;
            exports.entries.push_back(ExportEntry{
                "Fn" + std::to_string(i), static_cast<u16>(i + 1),
                [&called, id](Marshaler&) { called = id; },
                static_cast<u16>(4 * ((i + generation) % 8))});
        }
        ASSERT_TRUE(registry->register_dll("cache.dll", std::move(exports)).is_ok());
        for (int round = 0; round < 2; ++round) {
            for (int i = 0; i < kFunctions; ++i) {
                const auto addr = registry->get_thunk_address(
                    "cache.dll", "Fn" + std::to_string(i));
                ASSERT_TRUE(addr.is_ok());
                const auto handler = registry->resolve_handler(addr.value());
                ASSERT_TRUE(handler.is_ok());
                (*handler.value())(marshaler);
                EXPECT_EQ(called, generation * 1000 + i)
                    << "generation " << generation << " Fn" << i;
                EXPECT_EQ(registry->resolve_stdcall_bytes(addr.value()),
                          4 * ((i + generation) % 8));
            }
        }
        registry->~ThunkRegistry();
    }
}

TEST_F(ThunkRegistryTest, ResolveInvalidAddress) {
    auto result = registry.resolve_address(0xDEADBEEF);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::NotFound);
}

TEST_F(ThunkRegistryTest, ApiIdPackUnpack) {
    ApiId original{0x1234, 0x5678};
    u32 packed = original.pack();
    ApiId unpacked = ApiId::unpack(packed);
    
    EXPECT_EQ(unpacked.dll_id, original.dll_id);
    EXPECT_EQ(unpacked.function_id, original.function_id);
}

TEST_F(ThunkRegistryTest, CustomThunkSpacing) {
    ThunkRegistry custom_registry(0x80000000, 32);  // 32-byte spacing
    
    DllExports exports;
    exports.entries.push_back(ExportEntry{"Func1", 1, [](Marshaler&) {}});
    exports.entries.push_back(ExportEntry{"Func2", 2, [](Marshaler&) {}});
    custom_registry.register_dll("test.dll", std::move(exports));
    
    auto addr1 = custom_registry.get_thunk_address("test.dll", "Func1");
    auto addr2 = custom_registry.get_thunk_address("test.dll", "Func2");
    
    ASSERT_TRUE(addr1.is_ok());
    ASSERT_TRUE(addr2.is_ok());
    EXPECT_EQ(addr2.value() - addr1.value(), 32u);
}

TEST_F(ThunkRegistryTest, MultipleDlls) {
    DllExports kernel_exports;
    kernel_exports.entries.push_back(ExportEntry{"GetLastError", 1, [](Marshaler&) {}});
    registry.register_dll("kernel32.dll", std::move(kernel_exports));
    
    DllExports user_exports;
    user_exports.entries.push_back(ExportEntry{"GetMessage", 10, [](Marshaler&) {}});
    registry.register_dll("user32.dll", std::move(user_exports));
    
    EXPECT_EQ(registry.dll_count(), 2u);
    EXPECT_EQ(registry.function_count(), 2u);
    
    EXPECT_TRUE(registry.has_dll("kernel32.dll"));
    EXPECT_TRUE(registry.has_dll("user32.dll"));
    
    auto kernel_addr = registry.get_thunk_address("kernel32.dll", "GetLastError");
    auto user_addr = registry.get_thunk_address("user32.dll", "GetMessage");
    
    EXPECT_TRUE(kernel_addr.is_ok());
    EXPECT_TRUE(user_addr.is_ok());
    EXPECT_NE(kernel_addr.value(), user_addr.value());
}

// =============================================================================
// Missing-export stub contract
// =============================================================================
//
// A synthetic stub for an unimplemented import must hand the guest a value the
// caller reads as an EXPLICIT FAILURE under its own return convention. The
// legacy 0xFFFFFFFF is a failure for HRESULT/NTSTATUS/LSTATUS/errno_t but reads
// as TRUE for BOOL, so it is kept only where the convention is unknown.

TEST(MissingExportStubTest, DefaultsToLegacyFailureWhenConventionIsUnknown) {
    // Unknowable convention: keep the value every currently-working app sees.
    EXPECT_EQ(ThunkRegistry::missing_export_return_value("kernel32.dll",
                                                         "SomeUnknownApi"),
              0xFFFFFFFFull);
    // Ordinal-only imports carry no name to infer from.
    EXPECT_EQ(ThunkRegistry::missing_export_return_value("vendor.dll", ""),
              0xFFFFFFFFull);
    // Rtl* is deliberately NOT treated as NTSTATUS (BOOLEAN/pointer/void too).
    EXPECT_EQ(ThunkRegistry::missing_export_return_value("ntdll.dll",
                                                         "RtlSomething"),
              0xFFFFFFFFull);
}

TEST(MissingExportStubTest, HResultSurfacesReturnENotImpl) {
    // pathcch.h: every PathCch*/PathAlloc* export returns HRESULT.
    EXPECT_EQ(ThunkRegistry::missing_export_return_value(
                  "api-ms-win-core-path-l1-1-0.dll", "PathCchCombineEx"),
              0x80004001ull);
    EXPECT_EQ(ThunkRegistry::missing_export_return_value("pathcch.dll",
                                                         "PathAllocCombine"),
              0x80004001ull);
    // PathIsUNCEx is the BOOL export in that header; it must NOT be reclassified.
    EXPECT_EQ(ThunkRegistry::missing_export_return_value("pathcch.dll",
                                                         "PathIsUNCEx"),
              0xFFFFFFFFull);
    // In-proc server entry points are HRESULT by contract.
    EXPECT_EQ(ThunkRegistry::missing_export_return_value("vendor.dll",
                                                         "DllGetClassObject"),
              0x80004001ull);
}

TEST(MissingExportStubTest, NtdllSystemServicesReturnStatusNotImplemented) {
    EXPECT_EQ(ThunkRegistry::missing_export_return_value("ntdll.dll",
                                                         "NtQuerySomething"),
              0xC0000002ull);
    EXPECT_EQ(ThunkRegistry::missing_export_return_value("ntdll.dll",
                                                         "ZwQuerySomething"),
              0xC0000002ull);
    // The Nt/Zw rule is ntdll-only: a vendor DLL may name anything "Nt...".
    EXPECT_EQ(ThunkRegistry::missing_export_return_value("vendor.dll",
                                                         "NtQuerySomething"),
              0xFFFFFFFFull);
    // "Nt" must be followed by an uppercase letter, not lowercase prose.
    EXPECT_EQ(ThunkRegistry::missing_export_return_value("ntdll.dll", "Ntdll"),
              0xFFFFFFFFull);
}

TEST(MissingExportStubTest, StubWritesClassifiedFailureIntoGuestReturnRegister) {
    ThunkRegistry registry{0x70000000};
    const GuestAddr stub =
        registry.register_missing_export("api-ms-win-core-path-l1-1-0.dll",
                                         "PathCchCombineEx", 0);
    ASSERT_NE(stub, 0u);

    auto handler = registry.resolve_handler(stub);
    ASSERT_TRUE(handler.is_ok());

    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.gpr.rax = 0x1111111111111111ULL;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    (*handler.value())(marshaler);

    // FAILED(hr) must hold: the guest takes its error path instead of using a
    // garbage out-parameter it believes was filled in.
    const u32 hr = static_cast<u32>(cpu.gpr.rax & 0xFFFFFFFFu);
    EXPECT_EQ(hr, 0x80004001u);
    EXPECT_NE(hr & 0x80000000u, 0u);
}

TEST(MissingExportStubTest, CallsAreCountedAndDedupedAcrossImporters) {
    ThunkRegistry registry{0x70000000};
    const GuestAddr a =
        registry.register_missing_export("KERNEL32.DLL", "MissingThing", 0);
    const GuestAddr b =
        registry.register_missing_export("kernel32.dll", "MissingThing", 0);
    EXPECT_EQ(a, b) << "one stub per dll!func, shared by every importer";

    // Merely importing a symbol is harmless; only a CALL is a defect.
    EXPECT_EQ(registry.missing_export_call_count("kernel32.dll", "MissingThing"),
              0u);

    auto handler = registry.resolve_handler(a);
    ASSERT_TRUE(handler.is_ok());
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    for (int i = 0; i < 3; ++i) (*handler.value())(marshaler);

    EXPECT_EQ(registry.missing_export_call_count("KERNEL32.dll", "MissingThing"),
              3u);
    EXPECT_EQ(cpu.gpr.rax, 0xFFFFFFFFull);
    EXPECT_EQ(registry.missing_export_call_count("kernel32.dll", "NeverStubbed"),
              0u);
}

// =============================================================================
// Marshaler Tests
// =============================================================================

class MarshalerTest : public ::testing::Test {
protected:
    static constexpr GuestAddr TEST_BASE = 0x10000;
    static constexpr usize TEST_SIZE = 0x4000;
    
    void SetUp() override {
        auto result = memory.map_region(TEST_BASE, TEST_SIZE, MemoryProtection::ReadWrite);
        ASSERT_TRUE(result.is_ok()) << "Failed to map test region";
    }
    
    GuestMemory memory;
};

TEST_F(MarshalerTest, ReadAnsiString) {
    // Write "Hello" manually
    const char* test_str = "Hello";
    for (size_t i = 0; i <= 5; ++i) {  // Include null terminator
        memory.write_u8(TEST_BASE + i, static_cast<u8>(test_str[i]));
    }
    
    Marshaler marshaler(memory);
    auto result = marshaler.read_ansi_string(TEST_BASE);
    
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "Hello");
}

TEST_F(MarshalerTest, ReadAnsiStringWithMaxLength) {
    // Write "Hello World" manually
    const char* test_str = "Hello World";
    for (size_t i = 0; i <= 11; ++i) {
        memory.write_u8(TEST_BASE + i, static_cast<u8>(test_str[i]));
    }
    
    Marshaler marshaler(memory);
    auto result = marshaler.read_ansi_string(TEST_BASE, 5);
    
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "Hello");
}

TEST_F(MarshalerTest, ReadAnsiStringNullPointer) {
    Marshaler marshaler(memory);
    auto result = marshaler.read_ansi_string(0);
    
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::InvalidAddress);
}

TEST_F(MarshalerTest, WriteAnsiString) {
    Marshaler marshaler(memory);
    auto result = marshaler.write_ansi_string(TEST_BASE, "Test");
    
    EXPECT_TRUE(result.is_ok());
    
    // Verify written bytes
    EXPECT_EQ(memory.read_u8(TEST_BASE + 0).value(), 'T');
    EXPECT_EQ(memory.read_u8(TEST_BASE + 1).value(), 'e');
    EXPECT_EQ(memory.read_u8(TEST_BASE + 2).value(), 's');
    EXPECT_EQ(memory.read_u8(TEST_BASE + 3).value(), 't');
    EXPECT_EQ(memory.read_u8(TEST_BASE + 4).value(), 0);  // Null terminator
}

TEST_F(MarshalerTest, ReadWideStringAscii) {
    // Write "Hi" as UTF-16LE: 'H' = 0x0048, 'i' = 0x0069, null = 0x0000
    memory.write_u16(TEST_BASE + 0, 0x0048);  // 'H'
    memory.write_u16(TEST_BASE + 2, 0x0069);  // 'i'
    memory.write_u16(TEST_BASE + 4, 0x0000);  // null
    
    Marshaler marshaler(memory);
    auto result = marshaler.read_wide_string(TEST_BASE);
    
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "Hi");
}

TEST_F(MarshalerTest, ReadWideStringNonAscii) {
    // Write "é" (U+00E9) as UTF-16LE
    memory.write_u16(TEST_BASE + 0, 0x00E9);  // 'é'
    memory.write_u16(TEST_BASE + 2, 0x0000);  // null
    
    Marshaler marshaler(memory);
    auto result = marshaler.read_wide_string(TEST_BASE);
    
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "\xC3\xA9");  // UTF-8 for 'é'
}

// Audit 04_files X1: W APIs take UTF-16 names and any non-BMP character is
// valid (Learn: Naming Files). A surrogate pair is ONE code point (4-byte
// UTF-8), so a name FindFirstFileW returned round-trips into CreateFileW
// (files_meta_probe/files_ns_probe: U+1F600 and U+65E5 names).
TEST_F(MarshalerTest, ReadWideStringDecodesSurrogatePairs) {
    const vitreo::u16 units[] = {0x004A, 0x00E9, 0xD83D, 0xDE00, 0x65E5, 0};
    for (int i = 0; i < 6; ++i) memory.write_u16(TEST_BASE + i * 2, units[i]);
    Marshaler marshaler(memory);
    auto result = marshaler.read_wide_string(TEST_BASE);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "J\xC3\xA9\xF0\x9F\x98\x80\xE6\x97\xA5");
    // Round trip through the write direction yields the same UTF-16 units.
    ASSERT_TRUE(marshaler.write_wide_string(TEST_BASE + 0x100, result.value()).is_ok());
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(memory.read_u16(TEST_BASE + 0x100 + i * 2).value(), units[i]);
    }
}

TEST_F(MarshalerTest, ReadWideStringUnpairedSurrogateIsReplacementChar) {
    // An unpaired surrogate has no UTF-8 form (and no APFS name); it must
    // not produce invalid UTF-8 that the VFS would reinterpret as ANSI.
    const vitreo::u16 units[] = {0x0061, 0xDC00, 0x00E9, 0xD800, 0};
    for (int i = 0; i < 5; ++i) memory.write_u16(TEST_BASE + i * 2, units[i]);
    Marshaler marshaler(memory);
    auto result = marshaler.read_wide_string(TEST_BASE);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "a\xEF\xBF\xBD\xC3\xA9\xEF\xBF\xBD");
}

TEST_F(MarshalerTest, WriteWideStringAscii) {
    Marshaler marshaler(memory);
    auto result = marshaler.write_wide_string(TEST_BASE, "AB");
    
    EXPECT_TRUE(result.is_ok());
    
    // Verify UTF-16LE encoding
    EXPECT_EQ(memory.read_u16(TEST_BASE + 0).value(), 0x0041);  // 'A'
    EXPECT_EQ(memory.read_u16(TEST_BASE + 2).value(), 0x0042);  // 'B'
    EXPECT_EQ(memory.read_u16(TEST_BASE + 4).value(), 0x0000);  // null
}

TEST_F(MarshalerTest, WriteWideStringNonAscii) {
    Marshaler marshaler(memory);
    // Write 'é' (U+00E9) as UTF-8: 0xC3 0xA9
    auto result = marshaler.write_wide_string(TEST_BASE, "\xC3\xA9");
    
    EXPECT_TRUE(result.is_ok());
    
    // Verify UTF-16LE encoding
    EXPECT_EQ(memory.read_u16(TEST_BASE + 0).value(), 0x00E9);  // 'é'
    EXPECT_EQ(memory.read_u16(TEST_BASE + 2).value(), 0x0000);  // null
}

TEST_F(MarshalerTest, ReadWriteStruct) {
    struct TestStruct {
        u32 a;
        u16 b;
        u8 c;
        u8 d;
    };
    
    TestStruct original{0x12345678, 0xABCD, 0xEF, 0x01};
    
    Marshaler marshaler(memory);
    auto write_result = marshaler.write_struct(TEST_BASE, original);
    EXPECT_TRUE(write_result.is_ok());
    
    auto read_result = marshaler.read_struct<TestStruct>(TEST_BASE);
    EXPECT_TRUE(read_result.is_ok());
    
    TestStruct& read_val = read_result.value();
    EXPECT_EQ(read_val.a, original.a);
    EXPECT_EQ(read_val.b, original.b);
    EXPECT_EQ(read_val.c, original.c);
    EXPECT_EQ(read_val.d, original.d);
}

TEST_F(MarshalerTest, ReadWritePrimitives) {
    Marshaler marshaler(memory);
    
    // u8
    EXPECT_TRUE(marshaler.write_u8(TEST_BASE, 0xAB).is_ok());
    EXPECT_EQ(marshaler.read_u8(TEST_BASE).value(), 0xAB);
    
    // u16
    EXPECT_TRUE(marshaler.write_u16(TEST_BASE + 16, 0xCDEF).is_ok());
    EXPECT_EQ(marshaler.read_u16(TEST_BASE + 16).value(), 0xCDEF);
    
    // u32
    EXPECT_TRUE(marshaler.write_u32(TEST_BASE + 32, 0x12345678).is_ok());
    EXPECT_EQ(marshaler.read_u32(TEST_BASE + 32).value(), 0x12345678u);
    
    // u64
    EXPECT_TRUE(marshaler.write_u64(TEST_BASE + 64, 0x123456789ABCDEF0ULL).is_ok());
    EXPECT_EQ(marshaler.read_u64(TEST_BASE + 64).value(), 0x123456789ABCDEF0ULL);
}

TEST_F(MarshalerTest, MemoryAccessor) {
    Marshaler marshaler(memory);
    
    // Verify we can access the underlying memory
    EXPECT_EQ(&marshaler.memory(), &memory);
    
    const Marshaler& const_marshaler = marshaler;
    EXPECT_EQ(&const_marshaler.memory(), &memory);
}
