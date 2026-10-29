/// @file test_com_wait_run.cpp
/// @brief CoWaitForMultipleHandles and OleRun, x86 and x64.
///
/// Expected HRESULTs and *lpdwindex values are Windows 11's, recorded by
/// tools/oracle_probe/cowait_olerun_probe.c in
/// tests/oracle_data/cowait_olerun_probe{,.x86}.expected (identical for both
/// bitnesses). CoWaitForMultipleHandles is built on Vitreo's own
/// WaitForMultipleObjectsEx / MsgWaitForMultipleObjectsEx / PeekMessageW, which
/// have their own suites; here those exports are stand-ins whose results the
/// test scripts, so every outcome Windows maps can be driven exactly.

#include "com_test_common.hpp"
#include <winapi/kernel32/error.hpp>

#include <deque>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 S_OK_HR = 0x00000000u;
constexpr u32 S_FALSE_HR = 0x00000001u;
constexpr u32 E_INVALIDARG_HR = 0x80070057u;
constexpr u32 E_NOTIMPL_HR = 0x80004001u;
constexpr u32 RPC_S_CALLPENDING = 0x80010115u;
constexpr u32 RPC_E_NO_SYNC = 0x80010120u;
constexpr u32 CO_E_NOT_SUPPORTED = 0x80004021u;
constexpr u32 IDX_FILL = 0xDEADBEEFu;

struct Outcome {
    u32 result;
    std::optional<u32> last_error;
};

class ComWaitRunTest : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr DATA = 0x00100000;
    static constexpr GuestAddr STACK = 0x00200000;
    static constexpr GuestAddr HANDLES = DATA + 0x100;
    static constexpr GuestAddr INDEX = DATA + 0x200;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(ComCoreManager::COM_SCRATCH_BASE, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        if (x64()) {
            cpu.mode = cpu::CpuMode::Long;
        } else {
            cpu.init_protected_mode();
        }
        ASSERT_TRUE(registry.register_dll("ole32.dll", create_com_core_thunks(manager)).is_ok());
        ASSERT_TRUE(registry.register_dll("kernel32.dll", stand_ins({"WaitForMultipleObjectsEx"})).is_ok());
        ASSERT_TRUE(registry.register_dll(
            "user32.dll", stand_ins({"MsgWaitForMultipleObjectsEx", "PeekMessageW"})).is_ok());
        manager.set_thunk_registry(&registry);
        wfmo = registry.get_thunk_address("kernel32.dll", "WaitForMultipleObjectsEx").value();
        msgwait = registry.get_thunk_address("user32.dll", "MsgWaitForMultipleObjectsEx").value();
        peek = registry.get_thunk_address("user32.dll", "PeekMessageW").value();
        exports = create_com_core_thunks(manager);
        manager.set_guest_call([this](GuestAddr fn, const u64* args, usize n, bool) -> u64 {
            calls.push_back({fn, std::vector<u64>(args, args + n)});
            if (fn == peek) return peek_result;
            if (fn == wfmo || fn == msgwait) {
                if (script.empty()) return 0xFFFFFFFFu;
                const Outcome o = script.front();
                script.pop_front();
                if (o.last_error) set_last_error(*o.last_error);
                return o.result;
            }
            return guest_object_call(fn, args, n);
        });
        (void)memory.write_u32(HANDLES, 0x40);
        (void)memory.write_u32(HANDLES + 4, 0x44);
        (void)memory.write_u32(INDEX, IDX_FILL);
    }

    [[nodiscard]] bool x64() const { return GetParam(); }

    static DllExports stand_ins(std::initializer_list<const char*> names) {
        DllExports e;
        for (const char* n : names) e.entries.push_back({n, 0, [](Marshaler&) {}, 0});
        return e;
    }

    u32 call(const char* name, std::initializer_list<u64> args) {
        const ExportEntry* entry = nullptr;
        for (const auto& e : exports.entries) if (e.name == name) entry = &e;
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 0xFFFFFFFFu;
        const GuestAddr frame = STACK + 0x400;
        cpu.set_sp(frame);
        usize i = 0;
        for (u64 v : args) {
            if (!x64()) (void)memory.write_u32(frame + 4 + 4 * static_cast<GuestAddr>(i), static_cast<u32>(v));
            else if (i == 0) cpu.gpr.rcx = v;
            else if (i == 1) cpu.gpr.rdx = v;
            else if (i == 2) cpu.gpr.r8 = v;
            else if (i == 3) cpu.gpr.r9 = v;
            else (void)memory.write_u64(frame + 0x28 + 8 * static_cast<GuestAddr>(i - 4), v);
            ++i;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    u32 cowait(u32 flags, u32 timeout, u32 count, GuestAddr handles, GuestAddr index) {
        return call("CoWaitForMultipleHandles", {flags, timeout, count, handles, index});
    }

    [[nodiscard]] u32 index() { return memory.read_u32(INDEX).value(); }

    usize calls_to(GuestAddr fn) const {
        usize n = 0;
        for (const auto& c : calls) n += c.fn == fn;
        return n;
    }

    void enter_sta() { ASSERT_EQ(call("CoInitializeEx", {0, 2}), S_OK_HR); }

    // ---- a guest object for OleRun: IUnknown + IRunnableObject ----
    static constexpr GuestAddr OBJ = DATA + 0x1000, OBJ_VTBL = DATA + 0x1100;
    static constexpr GuestAddr RUN_IFACE = DATA + 0x1200, RUN_VTBL = DATA + 0x1300;
    static constexpr GuestAddr kQi = 0x401000, kAddRef = 0x402000, kRelease = 0x403000;
    static constexpr GuestAddr kRunQi = 0x404000, kRun = 0x405000;

    void build_object() {
        const GuestAddr ps = x64() ? 8 : 4;
        auto put = [&](GuestAddr at, GuestAddr v) {
            if (x64()) (void)memory.write_u64(at, v); else (void)memory.write_u32(at, static_cast<u32>(v));
        };
        put(OBJ, OBJ_VTBL);
        put(OBJ_VTBL, kQi);
        put(OBJ_VTBL + ps, kAddRef);
        put(OBJ_VTBL + 2 * ps, kRelease);
        put(RUN_IFACE, RUN_VTBL);
        put(RUN_VTBL, kRunQi);
        put(RUN_VTBL + ps, kAddRef);
        put(RUN_VTBL + 2 * ps, kRelease);
        put(RUN_VTBL + 3 * ps, 0x406000);  // GetRunningClass
        put(RUN_VTBL + 4 * ps, kRun);
    }

    u64 guest_object_call(GuestAddr fn, const u64* args, usize n) {
        if (fn == kQi) {
            ++qis;
            // IID_IRunnableObject {00000126-0000-0000-C000-000000000046}
            const u32 d1 = memory.read_u32(static_cast<GuestAddr>(args[1])).value();
            if (!has_runnable || d1 != 0x00000126u) {
                if (x64()) (void)memory.write_u64(static_cast<GuestAddr>(args[2]), 0);
                else (void)memory.write_u32(static_cast<GuestAddr>(args[2]), 0);
                return 0x80004002u;  // E_NOINTERFACE
            }
            ++refs;
            if (x64()) (void)memory.write_u64(static_cast<GuestAddr>(args[2]), RUN_IFACE);
            else (void)memory.write_u32(static_cast<GuestAddr>(args[2]), RUN_IFACE);
            return S_OK_HR;
        }
        if (fn == kAddRef) return ++refs;
        if (fn == kRelease) return --refs;
        if (fn == kRun) {
            ++runs;
            run_this = args[0];
            run_arg = n > 1 ? args[1] : 0xFFFF;
            return run_result;
        }
        return 0x80004001u;
    }

    struct Call {
        GuestAddr fn;
        std::vector<u64> args;
    };

    GuestMemory memory;
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_manager{vmm, memory, handles};
    ComTaskMemoryManager task_memory{heap_manager};
    cpu::CpuState cpu;
    ComCoreManager manager{task_memory};
    ThunkRegistry registry{0x70000000};
    DllExports exports;
    GuestAddr wfmo = 0, msgwait = 0, peek = 0;
    std::deque<Outcome> script;
    std::vector<Call> calls;
    u64 peek_result = 0;
    bool has_runnable = true;
    int qis = 0, refs = 1, runs = 0;
    u64 run_this = 0, run_arg = 0;
    u32 run_result = S_OK_HR;
};

// none_null_index / none_null_handles / none_flag_0x20 / none_count0 ...
TEST_P(ComWaitRunTest, ArgumentChecksAndTheirIndexWrites) {
    EXPECT_EQ(cowait(0, 0, 1, HANDLES, 0), E_INVALIDARG_HR);
    EXPECT_EQ(cowait(0, 0, 1, 0, INDEX), E_INVALIDARG_HR);
    EXPECT_EQ(index(), 0u) << "the failure writes 0";
    (void)memory.write_u32(INDEX, IDX_FILL);
    EXPECT_EQ(cowait(0x20, 0, 1, HANDLES, INDEX), E_INVALIDARG_HR);
    EXPECT_EQ(index(), 0u);
    (void)memory.write_u32(INDEX, IDX_FILL);
    EXPECT_EQ(cowait(0x80000000u, 0, 1, HANDLES, INDEX), E_INVALIDARG_HR);
    (void)memory.write_u32(INDEX, IDX_FILL);
    EXPECT_EQ(cowait(0, 0, 0, HANDLES, INDEX), RPC_E_NO_SYNC);
    EXPECT_EQ(index(), 0u);
    EXPECT_EQ(cowait(0, 0, 0, 0, INDEX), E_INVALIDARG_HR) << "NULL handles before count 0";
    EXPECT_EQ(cowait(0x20, 0, 0, HANDLES, INDEX), E_INVALIDARG_HR) << "bad flags before count 0";
    EXPECT_TRUE(calls.empty()) << "no wait for a rejected call";
}

TEST_P(ComWaitRunTest, OutsideAnStaIsWaitForMultipleObjectsEx) {
    script = {{0, std::nullopt}};
    EXPECT_EQ(cowait(0, 0, 1, HANDLES, INDEX), S_OK_HR);  // none_signaled
    EXPECT_EQ(index(), 0u);
    script = {{1, std::nullopt}};
    EXPECT_EQ(cowait(0, 0, 2, HANDLES, INDEX), S_OK_HR);  // none_second_of_two
    EXPECT_EQ(index(), 1u);
    script = {{0x102, std::nullopt}};
    EXPECT_EQ(cowait(0, 30, 1, HANDLES, INDEX), RPC_S_CALLPENDING);  // none_timeout30
    EXPECT_EQ(index(), 0x102u);
    // none_abandoned_second: the thread's stale error 0x12345678 -> 0x80075678.
    script = {{0x81, 0x12345678u}};
    EXPECT_EQ(cowait(0, 0, 2, HANDLES, INDEX), 0x80075678u);
    EXPECT_EQ(index(), 0x81u);
    // none_alertable_apc with ERROR_SUCCESS beforehand: S_OK.
    script = {{0xC0, 0u}};
    EXPECT_EQ(cowait(2, 100, 1, HANDLES, INDEX), S_OK_HR);
    EXPECT_EQ(index(), 0xC0u);
    // none_invalid_handle: HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE), WAIT_FAILED.
    script = {{0xFFFFFFFFu, 6u}};
    EXPECT_EQ(cowait(0, 0, 1, HANDLES, INDEX), 0x80070006u);
    EXPECT_EQ(index(), 0xFFFFFFFFu);

    // bWaitAll / bAlertable come from the two flags; the rest are ignored.
    calls.clear();
    script = {{0, std::nullopt}, {0, std::nullopt}, {0, std::nullopt}};
    (void)cowait(1, 7, 2, HANDLES, INDEX);
    (void)cowait(2, 7, 2, HANDLES, INDEX);
    (void)cowait(0x1F, 7, 2, HANDLES, INDEX);  // none_flags_all_valid
    ASSERT_EQ(calls.size(), 3u);
    const std::vector<u64> expect[3] = {{2, HANDLES, 1, 7, 0}, {2, HANDLES, 0, 7, 1}, {2, HANDLES, 1, 7, 1}};
    for (usize i = 0; i < 3; ++i) {
        EXPECT_EQ(calls[i].fn, wfmo);
        EXPECT_EQ(calls[i].args, expect[i]) << i;
    }
    EXPECT_EQ(calls_to(msgwait), 0u);
}

TEST_P(ComWaitRunTest, StaWaitDeliversOnlySentMessages) {
    enter_sta();
    calls.clear();
    // A sent message wakes the wait; it is delivered and the wait resumes.
    script = {{1, std::nullopt}, {0, std::nullopt}};
    EXPECT_EQ(cowait(3, 0xFFFFFFFFu, 1, HANDLES, INDEX), S_OK_HR);
    EXPECT_EQ(index(), 0u);
    ASSERT_EQ(calls_to(msgwait), 2u);
    ASSERT_EQ(calls_to(peek), 1u);
    for (const auto& c : calls) {
        if (c.fn == msgwait) {
            // (count, handles, timeout, QS_SENDMESSAGE, MWMO_WAITALL|MWMO_ALERTABLE)
            EXPECT_EQ(c.args[0], 1u);
            EXPECT_EQ(c.args[1], HANDLES);
            EXPECT_EQ(c.args[2], 0xFFFFFFFFu);
            EXPECT_EQ(c.args[3], 0x40u);
            EXPECT_EQ(c.args[4], 3u);
        }
        if (c.fn == peek) {
            EXPECT_EQ(c.args[1], 0u);
            EXPECT_EQ(c.args[4], 0x400000u) << "PM_NOREMOVE | PM_QS_SENDMESSAGE";
        }
    }
    EXPECT_EQ(calls_to(wfmo), 0u);
}

TEST_P(ComWaitRunTest, StaResultMapping) {
    enter_sta();
    script = {{0x102, std::nullopt}};
    EXPECT_EQ(cowait(0, 30, 1, HANDLES, INDEX), RPC_S_CALLPENDING);  // sta_timeout30
    EXPECT_EQ(index(), IDX_FILL) << "an STA timeout leaves the index";
    script = {{0x81, 0x12345678u}};
    EXPECT_EQ(cowait(0, 0, 2, HANDLES, INDEX), S_OK_HR);  // sta_abandoned_second
    EXPECT_EQ(index(), 0x81u);
    script = {{0xC0, std::nullopt}};
    EXPECT_EQ(cowait(2, 100, 1, HANDLES, INDEX), S_OK_HR);  // sta_alertable_apc
    EXPECT_EQ(index(), 0xC0u);
    (void)memory.write_u32(INDEX, IDX_FILL);
    script = {{0xFFFFFFFFu, 6u}};
    EXPECT_EQ(cowait(0, 0, 1, HANDLES, INDEX), RPC_S_CALLPENDING);  // sta_invalid_handle
    EXPECT_EQ(index(), IDX_FILL);
    EXPECT_EQ(get_last_error(), 6u);
    // sta1_*_flags0x10: window-message dispatch without call dispatch.
    calls.clear();
    EXPECT_EQ(cowait(0x10, 60, 1, HANDLES, INDEX), CO_E_NOT_SUPPORTED);
    EXPECT_EQ(index(), IDX_FILL);
    EXPECT_TRUE(calls.empty());
    // 0x18 is accepted and waits like flags 0 (sta1_*_flags0x18).
    script = {{0x102, std::nullopt}};
    EXPECT_EQ(cowait(0x18, 60, 1, HANDLES, INDEX), RPC_S_CALLPENDING);
}

TEST_P(ComWaitRunTest, StaInputAvailableWithQueuedInputFailsLoudly) {
    enter_sta();
    peek_result = 0;
    script = {{0x102, std::nullopt}};
    EXPECT_EQ(cowait(4, 0, 1, HANDLES, INDEX), RPC_S_CALLPENDING);  // sta_flag_inputavailable
    peek_result = 1;
    EXPECT_EQ(cowait(4, 60, 1, HANDLES, INDEX), E_NOTIMPL_HR);
}

TEST_P(ComWaitRunTest, OleRun) {
    build_object();
    EXPECT_EQ(call("OleRun", {0}), E_INVALIDARG_HR);  // olerun_null

    has_runnable = false;
    EXPECT_EQ(call("OleRun", {OBJ}), S_OK_HR);  // olerun_no_runnable
    EXPECT_EQ(runs, 0);
    EXPECT_EQ(refs, 1);

    has_runnable = true;
    for (u32 result : {S_OK_HR, 0x80041234u, S_FALSE_HR}) {  // olerun_runnable_*
        runs = 0;
        run_result = result;
        EXPECT_EQ(call("OleRun", {OBJ}), result);
        EXPECT_EQ(runs, 1);
        EXPECT_EQ(run_this, RUN_IFACE);
        EXPECT_EQ(run_arg, 0u) << "Run(NULL)";
        EXPECT_EQ(refs, 1) << "IRunnableObject released";
    }
}

INSTANTIATE_TEST_SUITE_P(Bitness, ComWaitRunTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

} // namespace
