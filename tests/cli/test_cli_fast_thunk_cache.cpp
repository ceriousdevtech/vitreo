/// @file test_cli_fast_thunk_cache.cpp
/// @brief Fast-thunk resolver, thread cache and lean-hot probe tests.

#include <gtest/gtest.h>
#include "runtime/execution/thunk_fast_path.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/msvcrt/io/crt_stdio.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

using namespace vitreo::cli;

TEST(FastThunkCacheTest, NestedResolveReusesThreadCache) {
    using vitreo::winapi::DllExports;
    using vitreo::winapi::ExportEntry;
    using vitreo::winapi::Marshaler;
    using vitreo::winapi::ThunkRegistry;

    ThunkRegistry registry(0x70000000);
    DllExports k32;
    auto stub = [](Marshaler&) {};
    for (const char* name : {"GetLastError", "SetLastError",
                             "GetCurrentThreadId", "GetCurrentProcessId",
                             "GetTickCount", "GetTickCount64",
                             "QueryPerformanceCounter", "QueryPerformanceFrequency",
                             "Sleep", "SleepEx", "TlsGetValue", "TlsSetValue",
                             "InterlockedIncrement", "InterlockedDecrement",
                             "InterlockedCompareExchange", "IsDebuggerPresent",
                             "HeapAlloc", "HeapFree", "WaitForSingleObject",
                             "ReleaseSemaphore", "GetOverlappedResult",
                             "EnterCriticalSection", "LeaveCriticalSection",
                             "TryEnterCriticalSection"}) {
        k32.entries.push_back(ExportEntry{name, 0, stub, 0});
    }
    ASSERT_TRUE(registry.register_dll("kernel32.dll", std::move(k32)).is_ok());
    DllExports winmm;
    winmm.entries.push_back(ExportEntry{"timeGetTime", 0, stub, 0});
    ASSERT_TRUE(registry.register_dll("winmm.dll", std::move(winmm)).is_ok());
    DllExports crt;
    crt.entries.push_back(ExportEntry{"fgetc", 0, stub, 4});
    crt.entries.push_back(ExportEntry{"ungetc", 0, stub, 8});
    ASSERT_TRUE(registry.register_dll("msvcrt.dll", std::move(crt)).is_ok());

    FastThunkAddrs first{};
    resolve_fast_thunks(first, registry);
    ASSERT_TRUE(first.resolved);
    ASSERT_NE(first.get_last_error, 0u);
    // After NOP-sled skip, cpu.rip is the dispatch gateway — not the stub base
    // returned by get_thunk_address.
    EXPECT_TRUE(registry.is_dispatch_address(first.get_last_error));
    EXPECT_TRUE(registry.is_dispatch_address(first.sleep));
    EXPECT_TRUE(registry.is_dispatch_address(first.time_get_time));
    EXPECT_NE(first.sleep, 0u);
    EXPECT_NE(first.query_performance_frequency, 0u);
    EXPECT_NE(first.tls_get_value, 0u);
    EXPECT_NE(first.interlocked_increment, 0u);
    EXPECT_NE(first.is_debugger_present, 0u);
    EXPECT_NE(first.enter_critical_section, 0u);
    EXPECT_NE(first.leave_critical_section, 0u);
    EXPECT_NE(first.try_enter_critical_section, 0u);
    EXPECT_NE(first.fgetc, 0u);
    EXPECT_NE(first.ungetc, 0u);
    EXPECT_TRUE(registry.is_dispatch_address(first.enter_critical_section));
    EXPECT_TRUE(registry.is_dispatch_address(first.leave_critical_section));
    EXPECT_TRUE(registry.is_dispatch_address(first.try_enter_critical_section));
    EXPECT_TRUE(is_lean_hot_thunk(first.sleep, first));
    EXPECT_TRUE(is_lean_hot_thunk(first.sleep_ex, first));
    EXPECT_TRUE(is_lean_hot_thunk(first.enter_critical_section, first));
    EXPECT_TRUE(is_lean_hot_thunk(first.leave_critical_section, first));
    EXPECT_TRUE(is_lean_hot_thunk(first.try_enter_critical_section, first));
    EXPECT_TRUE(is_lean_hot_thunk(first.fgetc, first));
    EXPECT_TRUE(is_lean_hot_thunk(first.ungetc, first));
    EXPECT_FALSE(is_lean_hot_thunk(first.get_tick_count, first));

    const auto gle_entry =
        registry.get_thunk_address("kernel32.dll", "GetLastError");
    ASSERT_TRUE(gle_entry.is_ok());
    EXPECT_TRUE(registry.is_stub_address(gle_entry.value()));
    EXPECT_EQ(first.get_last_error,
              gle_entry.value() + ThunkRegistry::STUB_SIZE);
    EXPECT_EQ(first.get_last_error,
              registry.dispatch_address_for_export(gle_entry.value()));

    FastThunkAddrs second{};
    resolve_fast_thunks(second, registry);
    EXPECT_TRUE(second.resolved);
    EXPECT_EQ(second.get_last_error, first.get_last_error);
    EXPECT_EQ(second.time_get_time, first.time_get_time);
    EXPECT_EQ(second.wait_for_single_object, first.wait_for_single_object);
    EXPECT_EQ(second.sleep, first.sleep);
    EXPECT_EQ(second.interlocked_compare_exchange,
              first.interlocked_compare_exchange);
    EXPECT_EQ(second.enter_critical_section, first.enter_critical_section);
    EXPECT_EQ(second.leave_critical_section, first.leave_critical_section);
    EXPECT_EQ(second.try_enter_critical_section,
              first.try_enter_critical_section);
    EXPECT_EQ(second.fgetc, first.fgetc);
    EXPECT_EQ(second.ungetc, first.ungetc);
}

TEST(FastThunkCacheTest, RegistryAddressReuseDoesNotReuseDestroyedHandlers) {
    using namespace vitreo;
    alignas(winapi::ThunkRegistry) unsigned char storage[sizeof(winapi::ThunkRegistry)];
    for (u32 expected : {41u, 73u}) {
        auto* registry = new (storage) winapi::ThunkRegistry(0x76000000 + expected * 0x1000);
        winapi::DllExports crt;
        crt.entries.push_back({"fgetc", 0, [expected](winapi::Marshaler& m) {
            m.set_return(expected);
        }});
        ASSERT_TRUE(registry->register_dll("msvcrt.dll", std::move(crt)).is_ok());
        FastThunkAddrs addrs;
        resolve_fast_thunks(addrs, *registry);
        GuestMemory memory;
        cpu::CpuState cpu;
        cpu.init_long_mode();
        cpu.rip = addrs.fgetc;
        ASSERT_TRUE(maybe_fast_thunk_call(cpu, memory, addrs));
        EXPECT_EQ(cpu.gpr.rax, expected);
        registry->~ThunkRegistry();
    }
}

TEST(FastThunkCacheTest, X64EaxReturnClearsThunkAddressUpperBits) {
    vitreo::winapi::ThunkRegistry registry(0x00007ffb00000000ULL);
    vitreo::winapi::DllExports k32;
    k32.entries.push_back({"TlsGetValue", 0, [](vitreo::winapi::Marshaler&) {}});
    ASSERT_TRUE(registry.register_dll("kernel32.dll", std::move(k32)).is_ok());
    vitreo::GuestMemory memory;
    FastThunkAddrs addrs{};
    resolve_fast_thunks(addrs, registry);
    const auto kTlsGetValue = addrs.tls_get_value;

    vitreo::cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = kTlsGetValue;
    cpu.gpr.rax = kTlsGetValue;
    cpu.gpr.rcx = 0;

    // With no TEB attached, TlsGetValue returns zero. An x64 EAX write must
    // clear RAX[63:32], rather than retaining 0x7ffb from the thunk gateway.
    ASSERT_TRUE(maybe_fast_thunk_call(cpu, memory, addrs));
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// Mono's JIT reads TEB.LastErrorValue (gs:[0x68]) directly after every
// SetLastError=true P/Invoke, so on x64 the TEB is the source of truth for
// GetLastError/SetLastError exactly as fs:[0x34] is on x86
// (tools/oracle_probe/files_core_probe.expected, "TEB.LastErrorValue mirror":
// a guest TEB write is what GetLastError returns, SetLastError lands in TEB).
TEST(FastThunkCacheTest, X64LastErrorLivesInTebLastErrorValue) {
    constexpr vitreo::GuestAddr kTeb = 0x30000;
    vitreo::winapi::ThunkRegistry registry(0x72000000);
    vitreo::winapi::DllExports k32;
    auto stub = [](vitreo::winapi::Marshaler&) {};
    k32.entries.push_back(vitreo::winapi::ExportEntry{"GetLastError", 0, stub, 0});
    k32.entries.push_back(vitreo::winapi::ExportEntry{"SetLastError", 0, stub, 0});
    ASSERT_TRUE(registry.register_dll("kernel32.dll", std::move(k32)).is_ok());
    FastThunkAddrs addrs{};
    resolve_fast_thunks(addrs, registry);
    ASSERT_TRUE(addrs.resolved);
    const vitreo::GuestAddr kGetLastError = addrs.get_last_error;
    const vitreo::GuestAddr kSetLastError = addrs.set_last_error;

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        kTeb, 0x2000, vitreo::MemoryProtection::ReadWrite).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.seg.gs_desc.base = kTeb;

    ASSERT_TRUE(memory.write_u32(kTeb + 0x68, 0x4321u).is_ok());
    vitreo::winapi::set_last_error(0);
    cpu.rip = kGetLastError;
    ASSERT_TRUE(maybe_fast_thunk_call(cpu, memory, addrs));
    EXPECT_EQ(cpu.gpr.rax, 0x4321u);

    cpu.rip = kSetLastError;
    cpu.gpr.rcx = 0x1234u;
    ASSERT_TRUE(maybe_fast_thunk_call(cpu, memory, addrs));
    EXPECT_EQ(memory.read_u32(kTeb + 0x68).value(), 0x1234u);
    EXPECT_EQ(vitreo::winapi::get_last_error(), 0x1234u);
}

TEST(FastThunkCacheTest, FgetcAndUngetcUseBoundStreamContract) {
    constexpr vitreo::GuestAddr kStack = 0x2ff00;

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    vitreo::winapi::CrtStdioManager stdio;
    vitreo::u32 seen_stream = 0;
    vitreo::i32 seen_character = 0;
    stdio.configure_guest_character_io(
        [&](vitreo::u32 stream) {
            seen_stream = stream;
            return static_cast<vitreo::i32>('Q');
        },
        [&](vitreo::i32 ch, vitreo::u32 stream) {
            seen_character = ch;
            seen_stream = stream;
            return ch;
        });
    vitreo::winapi::ThunkRegistry registry(0x74000000);
    vitreo::winapi::DllExports crt;
    crt.entries.push_back({"fgetc", 0, [&](vitreo::winapi::Marshaler& m) {
        m.set_return(static_cast<vitreo::u32>(stdio.guest_fgetc(m.arg(0))));
    }});
    crt.entries.push_back({"ungetc", 0, [&](vitreo::winapi::Marshaler& m) {
        m.set_return(static_cast<vitreo::u32>(stdio.guest_ungetc(m.arg(0), m.arg(1))));
    }});
    ASSERT_TRUE(registry.register_dll("msvcrt.dll", std::move(crt)).is_ok());
    FastThunkAddrs addrs{};
    resolve_fast_thunks(addrs, registry);
    const auto kFgetc = addrs.fgetc;
    const auto kUngetc = addrs.ungetc;

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kStack);
    ASSERT_TRUE(memory.write_u32(kStack + 4, 0x12345678u).is_ok());
    cpu.rip = kFgetc;
    EXPECT_TRUE(maybe_fast_thunk_call(cpu, memory, addrs));
    EXPECT_EQ(static_cast<vitreo::u32>(cpu.gpr.rax),
              static_cast<vitreo::u32>('Q'));
    EXPECT_EQ(seen_stream, 0x12345678u);

    ASSERT_TRUE(memory.write_u32(kStack + 4, static_cast<vitreo::u32>('Z')).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 8, 0x89abcdefu).is_ok());
    cpu.rip = kUngetc;
    EXPECT_TRUE(maybe_fast_thunk_call(cpu, memory, addrs));
    EXPECT_EQ(static_cast<vitreo::u32>(cpu.gpr.rax),
              static_cast<vitreo::u32>('Z'));
    EXPECT_EQ(seen_character, static_cast<vitreo::i32>('Z'));
    EXPECT_EQ(seen_stream, 0x89abcdefu);

}

TEST(FastThunkCacheTest, RegisteredCharacterFastPathKeepsItsCrtArenaOutsideAnExportScope) {
    using namespace vitreo;
    const auto directory = std::filesystem::temp_directory_path() / "vitreo_fast_stdio_arena";
    std::filesystem::create_directories(directory);
    { std::ofstream(directory / "input.bin", std::ios::binary) << "AB"; }
    for (bool x64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        winapi::CrtMemoryManager mm(memory);
        winapi::CrtDataBlock data;
        ASSERT_TRUE(data.initialize(memory));
        capsule::Vfs vfs;
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
        winapi::CrtStdioManager io;
        winapi::ThunkRegistry registry(0x78000000);
        auto legacy = winapi::create_crt_stdio_thunks(io, vfs, mm, data);
        auto ucrt = winapi::create_crt_stdio_thunks(io, vfs, mm, data,
            winapi::crt_stdio_buffer::Layout::UcrtOpaque32);
        ASSERT_TRUE(registry.register_dll("msvcrt.dll", std::move(legacy)).is_ok());
        ASSERT_TRUE(registry.register_dll("ucrtbase.dll", std::move(ucrt)).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(0x2f000);
        winapi::Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        ASSERT_TRUE(marshaler.write_ansi_string(0x22000, "C:\\input.bin").is_ok());
        ASSERT_TRUE(marshaler.write_ansi_string(0x22100, "rb").is_ok());
        const auto arguments = [&](std::initializer_list<u64> values) {
            usize i = 0;
            for (auto value : values) {
                if (!x64) (void)memory.write_u32(cpu.sp() + 4 + i * 4, static_cast<u32>(value));
                else if (i == 0) cpu.gpr.rcx = value;
                else if (i == 1) cpu.gpr.rdx = value;
                ++i;
            }
        };
        const auto call = [&](const char* dll, const char* name, std::initializer_list<u64> values) {
            arguments(values);
            const auto address = registry.get_thunk_address(dll, name);
            EXPECT_TRUE(address.is_ok());
            const auto handler = registry.resolve_handler(address.value());
            EXPECT_TRUE(handler.is_ok());
            (*handler.value())(marshaler);
            return static_cast<u32>(cpu.gpr.rax);
        };
        const auto first = call("msvcrt.dll", "fopen", {0x22000, 0x22100});
        const auto second = call("ucrtbase.dll", "fopen", {0x22000, 0x22100});
        ASSERT_NE(first, 0u);
        ASSERT_NE(second, 0u);
        FastThunkAddrs fast;
        resolve_fast_thunks(fast, registry);
        arguments({first}); cpu.rip = fast.fgetc;
        ASSERT_TRUE(maybe_fast_thunk_call(cpu, memory, fast));
        EXPECT_EQ(cpu.gpr.rax, 'A');
        EXPECT_EQ(call("ucrtbase.dll", "fgetc", {second}), 'A');
        arguments({'Q', first}); cpu.rip = fast.ungetc;
        ASSERT_TRUE(maybe_fast_thunk_call(cpu, memory, fast));
        EXPECT_EQ(cpu.gpr.rax, 'Q');
        arguments({first}); cpu.rip = fast.fgetc;
        ASSERT_TRUE(maybe_fast_thunk_call(cpu, memory, fast));
        EXPECT_EQ(cpu.gpr.rax, 'Q');
        EXPECT_EQ(call("ucrtbase.dll", "fgetc", {second}), 'B');
        EXPECT_EQ(call("msvcrt.dll", "fclose", {first}), 0u);
        EXPECT_EQ(call("ucrtbase.dll", "fclose", {second}), 0u);
    }
    std::filesystem::remove_all(directory);
}

TEST(FastThunkCacheTest, StoredAddrsAreDispatchGateways) {
    using vitreo::winapi::DllExports;
    using vitreo::winapi::ExportEntry;
    using vitreo::winapi::Marshaler;
    using vitreo::winapi::ThunkRegistry;

    ThunkRegistry registry(0x71000000);
    DllExports k32;
    auto stub = [](Marshaler&) {};
    k32.entries.push_back(ExportEntry{"GetTickCount", 0, stub, 0});
    k32.entries.push_back(ExportEntry{"GetLastError", 0, stub, 0});
    ASSERT_TRUE(registry.register_dll("kernel32.dll", std::move(k32)).is_ok());

    FastThunkAddrs addrs{};
    resolve_fast_thunks(addrs, registry);
    ASSERT_TRUE(addrs.resolved);

    const auto tick_stub =
        registry.get_thunk_address("kernel32.dll", "GetTickCount");
    const auto gle_stub =
        registry.get_thunk_address("kernel32.dll", "GetLastError");
    ASSERT_TRUE(tick_stub.is_ok());
    ASSERT_TRUE(gle_stub.is_ok());
    ASSERT_TRUE(registry.is_stub_address(tick_stub.value()));
    ASSERT_TRUE(registry.is_stub_address(gle_stub.value()));

    EXPECT_TRUE(registry.is_dispatch_address(addrs.get_tick_count));
    EXPECT_TRUE(registry.is_dispatch_address(addrs.get_last_error));
    EXPECT_EQ(addrs.get_tick_count,
              tick_stub.value() + ThunkRegistry::STUB_SIZE);
    EXPECT_EQ(addrs.get_last_error,
              gle_stub.value() + ThunkRegistry::STUB_SIZE);
    // Identity: already-dispatch input stays dispatch.
    EXPECT_EQ(registry.dispatch_address_for_export(addrs.get_tick_count),
              addrs.get_tick_count);
}

// ===========================================================================
// Fast-thunk dispatch is O(1)
//
// maybe_fast_thunk_call_inner is a 30-way chain of address compares and
// is_lean_hot_thunk a 19-way chain, both walked on EVERY thunk dispatch, so a
// dispatch bound for the registered-handler path paid all 49 before falling
// through. They are now short-circuited by an open-addressed probe. The probe
// MUST agree with the chains exactly: a wrong "lean hot" answer skips
// bookkeeping the slow path depends on, which is a correctness bug, not a
// slower path.
// ===========================================================================
TEST(FastThunkProbe, LeanHotClassificationMatchesTheLiteralSet) {
    constexpr vitreo::GuestAddr thunk_base = 0x71000000;
    vitreo::winapi::ThunkRegistry registry(thunk_base);

    auto make = [](std::initializer_list<const char*> names) {
        vitreo::winapi::DllExports e;
        vitreo::u16 ord = 1;
        for (const char* n : names) {
            e.entries.push_back(vitreo::winapi::ExportEntry{
                n, ord++,
                [](vitreo::winapi::Marshaler& m) { m.set_return(0); }});
        }
        return e;
    };
    ASSERT_TRUE(registry.register_dll("kernel32.dll", make({
        "GetLastError", "SetLastError", "GetCurrentThreadId", "GetTickCount",
        "GetTickCount64", "QueryPerformanceCounter",
        "QueryPerformanceFrequency", "GetCurrentProcessId", "Sleep", "SleepEx",
        "TlsGetValue", "TlsSetValue", "InterlockedIncrement",
        "InterlockedDecrement", "InterlockedCompareExchange",
        "IsDebuggerPresent", "HeapAlloc", "HeapFree", "WaitForSingleObject",
        "ReleaseSemaphore", "GetOverlappedResult", "SleepConditionVariableCS",
        "AcquireSRWLockExclusive", "ReleaseSRWLockExclusive",
        "TryAcquireSRWLockExclusive", "AcquireSRWLockShared",
        "ReleaseSRWLockShared", "TryAcquireSRWLockShared",
        "EnterCriticalSection", "LeaveCriticalSection",
        "TryEnterCriticalSection"})).is_ok());
    ASSERT_TRUE(registry.register_dll("msvcrt.dll",
                                      make({"fgetc", "ungetc"})).is_ok());

    vitreo::cli::FastThunkAddrs f;
    vitreo::cli::resolve_fast_thunks(f, registry);

    // Exactly the set is_lean_hot_thunk's literal chain answers true for.
    const std::vector<vitreo::GuestAddr> lean_hot{
        f.heap_alloc, f.heap_free, f.wait_for_single_object,
        f.release_semaphore, f.get_overlapped_result,
        f.sleep_condition_variable_cs, f.sleep, f.sleep_ex,
        f.enter_critical_section, f.leave_critical_section,
        f.try_enter_critical_section, f.acquire_srw_exclusive,
        f.release_srw_exclusive, f.try_acquire_srw_exclusive,
        f.acquire_srw_shared, f.release_srw_shared, f.try_acquire_srw_shared,
        f.fgetc, f.ungetc};
    for (vitreo::GuestAddr a : lean_hot) {
        ASSERT_NE(a, 0u);
        EXPECT_TRUE(vitreo::cli::is_lean_hot_thunk(a, f))
            << "lean-hot thunk 0x" << std::hex << a << " misclassified";
    }

    // Fast-path thunks that are deliberately NOT lean-hot: answering true here
    // would skip the callee-stack clobber the slow path performs.
    for (vitreo::GuestAddr a : {f.get_tick_count, f.tls_get_value,
                                f.query_performance_counter,
                                f.get_last_error, f.is_debugger_present}) {
        ASSERT_NE(a, 0u);
        EXPECT_FALSE(vitreo::cli::is_lean_hot_thunk(a, f))
            << "non-lean-hot thunk 0x" << std::hex << a
            << " wrongly reported lean-hot";
    }

    // Addresses that are not thunks at all must never match.
    for (vitreo::GuestAddr a : {vitreo::GuestAddr{0},
                                vitreo::GuestAddr{0x401000},
                                thunk_base - 8, f.heap_alloc + 4}) {
        EXPECT_FALSE(vitreo::cli::is_lean_hot_thunk(a, f))
            << "non-thunk 0x" << std::hex << a << " reported lean-hot";
    }
}
