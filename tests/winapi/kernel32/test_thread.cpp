/// @file test_thread.cpp
/// @brief Tests for thread creation and lifecycle (THREAD-001)

#include "vitreo/guest_sentinels.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/common/object_access_constants.hpp"
#include "winapi/common/thread_times_oracle.hpp"
#include "winapi/common/thread_description_contract.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/ab_cycle.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <pthread.h>
#include <sys/qos.h>
#include <thread>
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <optional>

using namespace vitreo;
using namespace vitreo::winapi;

// ---------------------------------------------------------------------------
// Per-test isolation of the current guest TID.
//
// ThreadManager::set_current_thread_id() writes a thread_local that is *not*
// owned by any ThreadManager instance: it survives the manager that a test
// created, and on the gtest main thread it survives the test itself. Any test
// anywhere in this binary that sets a synthetic TID (sync causality tracing,
// critical-section ownership, TLS/TEB probes, ...) and does not restore it
// therefore poisons every later test that reads the current TID.
//
// That is exactly how it presented: ThreadTest.GetCurrentThreadIdReturnsMainTid
// and ThreadTest.DuplicateCurrentThreadHandleReturnsRealThreadHandle passed
// alone and failed in the full run, because
// SyncCausalityTest.InfiniteEventWaitReportsCreationAndLastTransition runs
// earlier and leaves the main thread claiming to be guest TID 7.
//
// A process that has not created any guest thread is, per Windows, running as
// its initial thread, so restoring TID 1 after every test both bounds the leak
// to the test that made it and restores the correct default. The listener is
// installed for the whole binary, not just this fixture, because the leak
// poisons whatever test happens to run next.
namespace {
class CurrentThreadIdIsolationListener : public ::testing::EmptyTestEventListener {
    void OnTestEnd(const ::testing::TestInfo&) override {
        ThreadManager::set_current_thread_id(1);
    }
};

const int g_install_current_tid_isolation = [] {
    ::testing::UnitTest::GetInstance()->listeners().Append(
        new CurrentThreadIdIsolationListener);
    return 0;
}();
} // namespace

class ThreadTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_handles = std::make_unique<HandleTable>();
        m_mgr     = std::make_unique<ThreadManager>(*m_handles);
    }

    std::unique_ptr<HandleTable> m_handles;
    std::unique_ptr<ThreadManager> m_mgr;
};

// All expected statuses, limits and output/fault ordering come from
// thread_description_probe{,.x86}.expected, measured on native Windows.
class ThreadDescriptionTest : public ::testing::TestWithParam<bool> {
protected:
    GuestMemory memory;
    VirtualMemoryManager vmm{memory};
    HandleTable handles;
    HeapManager heap{vmm, memory, handles};
    GlobalLocalManager local{heap};
    ThreadManager threads{handles};
    capsule::Vfs vfs;
    Kernel32ExtManager manager{memory, vfs};
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    static constexpr GuestAddr output = 0x4000, text = 0x5000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x1000, 0x24000, MemoryProtection::ReadWrite).is_ok());
        if (GetParam()) cpu.init_long_mode();
        else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x3000;
        marshaler.attach_cpu(cpu);
        local.set_guest_64bit(GetParam());
        manager.set_handle_table(&handles);
        manager.set_thread_manager(&threads);
        manager.set_global_local_manager(&local);
        ThreadManager::set_current_thread_id(threads.main_thread_id());
    }
    u64 current() const { return GetParam() ? ~u64{1} : static_cast<u32>(-2); }
    void invoke(bool set, u64 handle, GuestAddr pointer, u32 hr, bool fault = false) {
        cpu.pending_raise.clear();
        if (GetParam()) { cpu.gpr.rcx = handle; cpu.gpr.rdx = pointer; }
        else {
            ASSERT_TRUE(memory.write_u32(0x3004, static_cast<u32>(handle)).is_ok());
            ASSERT_TRUE(memory.write_u32(0x3008, static_cast<u32>(pointer)).is_ok());
        }
        set_last_error(0xdead);
        if (set) manager.impl_SetThreadDescription(marshaler);
        else manager.impl_GetThreadDescription(marshaler);
        EXPECT_EQ(get_last_error(), 0xdeadu);
        EXPECT_EQ(cpu.pending_raise.active, fault);
        if (fault) {
            EXPECT_EQ(cpu.pending_raise.code, thread_description_contract::ACCESS_VIOLATION);
            EXPECT_EQ(cpu.pending_raise.num_args, 2u);
            EXPECT_EQ(cpu.pending_raise.args[0], set ? 0u : 1u);
        } else EXPECT_EQ(cpu.gpr.rax, hr);
    }
    void set(u64 handle, const std::u16string& value, u32 hr) {
        ASSERT_TRUE(memory.write_bytes(text, reinterpret_cast<const Byte*>(value.c_str()),
                                      (value.size()+1)*sizeof(char16_t)).is_ok());
        invoke(true, handle, text, hr);
    }
    void get(u64 handle, u32 hr, const std::u16string& expected = {}) {
        if (GetParam()) ASSERT_TRUE(memory.write_u64(output, 0x1234).is_ok());
        else ASSERT_TRUE(memory.write_u32(output, 0x1234).is_ok());
        invoke(false, handle, output, hr);
        const GuestAddr buffer = GetParam() ? memory.read_u64(output).value_or(0)
                                          : memory.read_u32(output).value_or(0);
        if (hr != thread_description_contract::NT_SUCCESS_HR) { EXPECT_EQ(buffer, 0u); return; }
        ASSERT_NE(buffer, 0u);
        for (usize index=0; index<=expected.size(); ++index)
            EXPECT_EQ(memory.read_u16(buffer+index*sizeof(u16)).value_or(0xffff),
                      index==expected.size() ? 0 : expected[index]);
        EXPECT_EQ(local.global_free(buffer), 0u);
    }
};

TEST_P(ThreadDescriptionTest, AccessWidthOutputsAndLifetimeMatchWindows) {
    namespace c = thread_description_contract;
    get(current(), c::NT_SUCCESS_HR);
    set(current(), u"main\u03c0", c::NT_SUCCESS_HR);
    get(current(), c::NT_SUCCESS_HR, u"main\u03c0");
    Handle main = threads.duplicate_thread_handle(ThreadManager::get_current_thread());
    ASSERT_NE(main, INVALID_HANDLE);
    auto* object = threads.get_thread_by_handle(main);
    ASSERT_NE(object, nullptr);
    struct Rights { u32 mask; bool get, set; };
    const Rights rows[] = {
        {0,false,false}, {c::THREAD_QUERY_INFORMATION,true,false},
        {c::THREAD_QUERY_LIMITED_INFORMATION,true,false},
        {c::THREAD_SET_INFORMATION,false,true}, {c::THREAD_SET_LIMITED_INFORMATION,false,true},
        {sync_access::SYNCHRONIZE,false,false},
        {c::THREAD_QUERY_LIMITED_INFORMATION|c::THREAD_SET_LIMITED_INFORMATION,true,true}};
    for (const auto& row : rows) {
        SCOPED_TRACE(row.mask);
        Handle handle = handles.allocate(HandleType::Thread, object, 0, row.mask);
        get(handle, row.get ? c::NT_SUCCESS_HR : c::NT_ACCESS_DENIED_HR, u"main\u03c0");
        set(handle, u"main\u03c0", row.set ? c::NT_SUCCESS_HR : c::NT_ACCESS_DENIED_HR);
        ASSERT_TRUE(handles.close(handle));
    }
    for (u64 invalid : {u64{0}, u64{0x12345678}}) {
        get(invalid, c::NT_INVALID_HANDLE_HR); set(invalid, u"x", c::NT_INVALID_HANDLE_HR);
    }
    const Handle event = handles.allocate(HandleType::Event, nullptr);
    for (u64 wrong_type : {GetParam() ? ~u64{0} : u64{UINT32_MAX}, u64{event}}) {
        get(wrong_type, c::NT_TYPE_MISMATCH_HR); set(wrong_type,u"x",c::NT_TYPE_MISMATCH_HR);
    }
    ASSERT_TRUE(handles.close(event));
    if (GetParam()) {
        for (u64 bad : {u64{main}|(u64{1}<<32), u64{0xfffffffe}, u64{0xffffffff}}) {
            get(bad,c::NT_INVALID_HANDLE_HR); set(bad,u"x",c::NT_INVALID_HANDLE_HR);
        }
    }
    invoke(true,current(),0,c::NT_SUCCESS_HR);
    get(current(),c::NT_SUCCESS_HR);
    u32 tid=0;
    Handle child=threads.create_thread(0,0x1000,0,CREATE_SUSPENDED,&tid);
    ASSERT_NE(child,INVALID_HANDLE);
    set(child,u"retained",c::NT_SUCCESS_HR); get(child,c::NT_SUCCESS_HR,u"retained");
    threads.exit_thread(tid,0);
    get(child,c::NT_SUCCESS_HR,u"retained"); set(child,u"after-exit",c::NT_SUCCESS_HR);
    get(child,c::NT_SUCCESS_HR,u"after-exit"); ASSERT_TRUE(handles.close(child));
    get(child,c::NT_INVALID_HANDLE_HR); set(child,u"x",c::NT_INVALID_HANDLE_HR);
    ASSERT_TRUE(handles.close(main));
}

TEST_P(ThreadDescriptionTest, PointerFaultsAndNameLimitMatchWindows) {
    namespace c = thread_description_contract;
    for (u64 handle : {current(),u64{0}}) {
        invoke(true,handle,1,0,true);
        EXPECT_EQ(cpu.pending_raise.args[1],1u);
        invoke(false,handle,0,0,true);
        EXPECT_EQ(cpu.pending_raise.args[1],0u);
    }
    ASSERT_TRUE(memory.map_region(0x40000,0x1000,MemoryProtection::ReadWrite).is_ok());
    const GuestAddr boundary=0x41000-sizeof(u32);
    ASSERT_TRUE(memory.write_u32(boundary,0x1234).is_ok());
    invoke(false,current(),boundary,c::NT_SUCCESS_HR,GetParam());
    if (GetParam()) {
        EXPECT_EQ(memory.read_u32(boundary).value_or(0),0x1234u);
        EXPECT_EQ(cpu.pending_raise.args[1],0x41000u);
    } else EXPECT_EQ(local.global_free(memory.read_u32(boundary).value_or(0)),0u);
    ASSERT_TRUE(memory.write_u16(0x40ffe,u'x').is_ok());
    invoke(true,current(),0x40ffe,0,true);
    EXPECT_EQ(cpu.pending_raise.args[1],0x41000u);
    ASSERT_TRUE(memory.map_region(0x42000,0x1000,MemoryProtection::Read).is_ok());
    invoke(false,current(),0x42000,0,true);
    EXPECT_EQ(cpu.pending_raise.args[1],0x42000u);
    invoke(false,current(),output+1,c::NT_SUCCESS_HR);
    const GuestAddr unaligned=GetParam() ? memory.read_u64(output+1).value_or(0)
                                       : memory.read_u32(output+1).value_or(0);
    EXPECT_EQ(local.global_free(unaligned),0u);
    const std::u16string longest(c::MAX_DESCRIPTION_CHARS,u'a');
    set(current(),longest,c::NT_SUCCESS_HR); get(current(),c::NT_SUCCESS_HR,longest);
    set(current(),longest+u'a',c::NT_NAME_TOO_LONG_HR);
    get(current(),c::NT_SUCCESS_HR,longest);
    set(current(),longest+u"aa",c::NT_NAME_TOO_LONG_HR);
    get(current(),c::NT_SUCCESS_HR,longest);
    ASSERT_TRUE(memory.map_region(0x50000,0x10000,MemoryProtection::ReadWrite).is_ok());
    const std::u16string unterminated(0x10000/sizeof(char16_t),u'a');
    ASSERT_TRUE(memory.write_bytes(0x50000,reinterpret_cast<const Byte*>(unterminated.data()),
                                  unterminated.size()*sizeof(char16_t)).is_ok());
    invoke(true,current(),0x50000,0,true);
    EXPECT_EQ(cpu.pending_raise.args[1],0x60000u);
    get(current(),c::NT_SUCCESS_HR,longest);
}

// A guest thread is a host thread: sample, xctrace and Instruments label it
// with the name the guest gave it, and only the calling thread can be named
// (macOS has no call that names another thread). The guest-visible contract
// (HRESULTs, GetThreadDescription) is the Learn/oracle one tested above and is
// unchanged by the host naming.
namespace {
std::string host_thread_name() {
    char name[64] = {};
    EXPECT_EQ(pthread_getname_np(pthread_self(), name, sizeof(name)), 0);
    return name;
}

struct RestoreHostThreadName {
    std::string saved = host_thread_name();
    ~RestoreHostThreadName() { (void)pthread_setname_np(saved.c_str()); }
};
} // namespace

TEST_P(ThreadDescriptionTest, NamesTheCallingHostThread) {
    namespace c = thread_description_contract;
    const RestoreHostThreadName restore;
    ASSERT_NE(host_thread_name(), "CrRendererMain");

    // The pseudo handle names the caller.
    set(current(), u"CrRendererMain", c::NT_SUCCESS_HR);
    EXPECT_EQ(host_thread_name(), "CrRendererMain");
    get(current(), c::NT_SUCCESS_HR, u"CrRendererMain");

    // So does a real handle to the calling thread; the name is UTF-8.
    const Handle self =
        threads.duplicate_thread_handle(ThreadManager::get_current_thread());
    ASSERT_NE(self, INVALID_HANDLE);
    set(self, u"Compositorπ€", c::NT_SUCCESS_HR);
    EXPECT_EQ(host_thread_name(), "Compositor\xcf\x80\xe2\x82\xac");
    get(self, c::NT_SUCCESS_HR, u"Compositorπ€");

    // A NULL description is the empty description: the host name is cleared.
    invoke(true, current(), 0, c::NT_SUCCESS_HR);
    EXPECT_EQ(host_thread_name(), "");
    ASSERT_TRUE(handles.close(self));
}

TEST_P(ThreadDescriptionTest, HostNameIsCutOnAUtf8CodePointBoundary) {
    namespace c = thread_description_contract;
    const RestoreHostThreadName restore;
    // macOS keeps MAXTHREADNAMESIZE - 1 = 63 bytes. A character that would
    // straddle the cut is dropped whole, never split into invalid UTF-8.
    struct Row { std::u16string description; std::string host; };
    const std::string a62(62, 'a'), a61(61, 'a'), a60(60, 'a');
    const Row rows[] = {
        {std::u16string(100, u'a'), std::string(63, 'a')},
        {std::u16string(62, u'a') + u"é", a62},                 // 64 bytes
        {std::u16string(61, u'a') + u"é", a61 + "\xc3\xa9"},    // 63 bytes
        {std::u16string(61, u'a') + u"€", a61},                 // 64 bytes
        {std::u16string(60, u'a') + u"€" + u"zzz", a60 + "\xe2\x82\xac"},
        {std::u16string(62, u'a') + u"\U0001F600", a62},             // 66 bytes
        {std::u16string(59, u'a') + u"\U0001F600" + u"z",
         std::string(59, 'a') + "\xf0\x9f\x98\x80"},               // 63 bytes
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.host);
        set(current(), row.description, c::NT_SUCCESS_HR);
        EXPECT_EQ(host_thread_name(), row.host);
        // The guest keeps the full description: only the host name is cut.
        get(current(), c::NT_SUCCESS_HR, row.description);
    }
    // An unpaired surrogate is not UTF-16 text a host name can carry; it
    // becomes U+FFFD rather than emptying or corrupting the name.
    set(current(), u"ab\xd800" u"cd", c::NT_SUCCESS_HR);
    EXPECT_EQ(host_thread_name(), "ab\xef\xbf\xbd" "cd");
}

TEST_P(ThreadDescriptionTest, OtherTargetsAndFailuresLeaveTheHostNameAlone) {
    namespace c = thread_description_contract;
    const RestoreHostThreadName restore;
    set(current(), u"before", c::NT_SUCCESS_HR);
    ASSERT_EQ(host_thread_name(), "before");

    // Another thread's description is stored for the guest but cannot name
    // the calling host thread.
    u32 tid = 0;
    const Handle child = threads.create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &tid);
    ASSERT_NE(child, INVALID_HANDLE);
    set(child, u"child-name", c::NT_SUCCESS_HR);
    get(child, c::NT_SUCCESS_HR, u"child-name");
    EXPECT_EQ(host_thread_name(), "before");
    threads.exit_thread(tid, 0);
    ASSERT_TRUE(handles.close(child));

    // A refused call (no THREAD_SET_LIMITED_INFORMATION) names nothing.
    const Handle self =
        threads.duplicate_thread_handle(ThreadManager::get_current_thread());
    ASSERT_NE(self, INVALID_HANDLE);
    auto* object = threads.get_thread_by_handle(self);
    ASSERT_NE(object, nullptr);
    const Handle query_only = handles.allocate(
        HandleType::Thread, object, 0, c::THREAD_QUERY_LIMITED_INFORMATION);
    set(query_only, u"denied", c::NT_ACCESS_DENIED_HR);
    EXPECT_EQ(host_thread_name(), "before");
    ASSERT_TRUE(handles.close(query_only));
    ASSERT_TRUE(handles.close(self));

    // A name over the Windows limit is refused and names nothing.
    set(current(), std::u16string(c::MAX_DESCRIPTION_CHARS + 1, u'a'),
        c::NT_NAME_TOO_LONG_HR);
    EXPECT_EQ(host_thread_name(), "before");
}

INSTANTIATE_TEST_SUITE_P(BothArchitectures,ThreadDescriptionTest,::testing::Bool());

// Expected values come from thread_id_access_probe{,.x86}.expected and
// process_id_of_thread_probe{,.x86}.expected recorded on Windows, not Vitreo.
static void check_thread_identity_contract(bool process_query) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x3000,
                                     MemoryProtection::ReadWrite).is_ok());
        capsule::Vfs vfs;
        HandleTable handles;
        ThreadManager threads(handles);
        ProcessManager process;
        ThreadManager::set_current_thread_id(threads.main_thread_id());
        Kernel32ExtManager manager(memory, vfs);
        manager.set_handle_table(&handles);
        manager.set_thread_manager(&threads);
        manager.set_process_manager(&process);
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode();
        else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x3000;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        auto query = [&](u64 handle, u32 id, u32 error) {
            SCOPED_TRACE(handle);
            if (x64) cpu.gpr.rcx = handle;
            else EXPECT_TRUE(memory.write_u32(0x3004, static_cast<u32>(handle)).is_ok());
            set_last_error(0xdead);
            if (process_query) manager.impl_GetProcessIdOfThread(marshaler);
            else manager.impl_GetThreadId(marshaler);
            EXPECT_EQ(cpu.gpr.rax, id);
            EXPECT_EQ(get_last_error(), error);
        };
        u32 thread_id = 0;
        const Handle thread = threads.create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &thread_id);
        const u32 id = process_query ? process.get_current_process_id() : thread_id;
        ASSERT_NE(thread, INVALID_HANDLE);
        query(thread, id, 0xdead);
        query(x64 ? ~u64{1} : static_cast<u32>(-2),
              process_query ? process.get_current_process_id() : threads.main_thread_id(), 0xdead);
        query(0, 0, error_codes::ERROR_INVALID_HANDLE);
        query(x64 ? ~u64{0} : static_cast<u32>(-1), 0, error_codes::ERROR_INVALID_HANDLE);
        query(0x12345678, 0, error_codes::ERROR_INVALID_HANDLE);
        for (HandleType type : {HandleType::Event, HandleType::Process}) {
            const Handle wrong_type = handles.allocate(type, nullptr);
            query(wrong_type, 0, error_codes::ERROR_INVALID_HANDLE);
            ASSERT_TRUE(handles.close(wrong_type));
        }
        const u32 full_query = ThreadManager::THREAD_QUERY_INFORMATION;
        const u32 limited_query = ThreadManager::THREAD_QUERY_LIMITED_INFORMATION;
        for (u32 access : {0u, full_query, limited_query, sync_access::SYNCHRONIZE,
                           full_query | sync_access::SYNCHRONIZE, HandleAccess::All}) {
            const Handle copy = threads.duplicate_thread_handle(thread, 0, access);
            ASSERT_NE(copy, INVALID_HANDLE);
            const bool permitted = (access & (full_query | limited_query)) != 0;
            query(copy, permitted ? id : 0,
                  permitted ? 0xdead : error_codes::ERROR_ACCESS_DENIED);
            ASSERT_TRUE(handles.close(copy));
        }
        if (x64) {
            query(static_cast<u64>(thread) | (u64{1} << 32), 0, error_codes::ERROR_INVALID_HANDLE);
            query(static_cast<u32>(-2), 0, error_codes::ERROR_INVALID_HANDLE);
        }
        threads.exit_thread(thread_id, 4);
        query(thread, id, 0xdead);
        ASSERT_TRUE(handles.close(thread));
        query(thread, 0, error_codes::ERROR_INVALID_HANDLE);
    }
}

TEST(GetThreadIdContractTest, MatchesNativeRightsWidthAndLifetimeInBothAbis) {
    check_thread_identity_contract(false);
}

TEST(GetProcessIdOfThreadContractTest, MatchesNativeRightsWidthAndLifetimeInBothAbis) {
    check_thread_identity_contract(true);
}

// ---------------------------------------------------------------------------
// 1. CreateThread returns a valid handle and thread ID
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, CreateThreadReturnsValidHandleAndTid) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0x42, 0, &tid);

    EXPECT_NE(h, INVALID_HANDLE);
    EXPECT_NE(tid, 0u);
    EXPECT_TRUE(m_handles->is_valid(h));
    EXPECT_EQ(m_handles->get_type(h), HandleType::Thread);
}

// ---------------------------------------------------------------------------
// 2. Thread has correct start address and parameter
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, ThreadHasCorrectStartAddrAndParam) {
    u32 tid = 0;
    m_mgr->create_thread(0, 0xDEAD, 0xBEEF, 0, &tid);

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_EQ(gt->start_address, 0xDEADu);
    EXPECT_EQ(gt->parameter, 0xBEEFu);
}

TEST_F(ThreadTest, ThreadTracksWindowsCreationAndExitTimes) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, 0, &tid);

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_NE(gt->creation_time, 0u);
    EXPECT_EQ(gt->exit_time, 0u);

    ASSERT_TRUE(m_mgr->terminate_thread(h, 0));
    EXPECT_GE(gt->exit_time, gt->creation_time);
}

// ---------------------------------------------------------------------------
// 3. GetExitCodeThread returns STILL_ACTIVE for a new thread
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, NewThreadExitCodeIsStillActive) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, 0, &tid);

    auto result = m_mgr->get_exit_code(h);
    ASSERT_FALSE(result.is_err());
    EXPECT_EQ(result.value(), STILL_ACTIVE);
}

// ---------------------------------------------------------------------------
// 4. CREATE_SUSPENDED flag sets suspended = true
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, CreateSuspendedFlagSetsSuspended) {
    u32 tid = 0;
    m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &tid);

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_GT(gt->suspend_count, 0u);
}

// ---------------------------------------------------------------------------
// 5. GetCurrentThread returns pseudo-handle (-2)
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, GetCurrentThreadReturnsPseudoHandle) {
    Handle h = ThreadManager::get_current_thread();
    EXPECT_EQ(h, static_cast<Handle>(-2));
}

TEST_F(ThreadTest, DuplicateCurrentThreadHandleReturnsRealThreadHandle) {
    Handle duplicated = m_mgr->duplicate_thread_handle(ThreadManager::get_current_thread());

    EXPECT_NE(duplicated, INVALID_HANDLE);
    EXPECT_NE(duplicated, ThreadManager::get_current_thread());
    EXPECT_TRUE(m_handles->is_valid(duplicated));
    EXPECT_EQ(m_handles->get_type(duplicated), HandleType::Thread);
    EXPECT_EQ(m_mgr->get_thread_by_handle(duplicated), m_mgr->get_thread(1));
}

TEST_F(ThreadTest, DuplicateRealThreadHandleReturnsAnotherRealThreadHandle) {
    u32 tid = 0;
    Handle original = m_mgr->create_thread(0, 0x1000, 0, 0, &tid);

    Handle duplicated = m_mgr->duplicate_thread_handle(original);

    EXPECT_NE(duplicated, INVALID_HANDLE);
    EXPECT_NE(duplicated, original);
    EXPECT_TRUE(m_handles->is_valid(duplicated));
    EXPECT_EQ(m_handles->get_type(duplicated), HandleType::Thread);
    EXPECT_EQ(m_mgr->get_thread_by_handle(duplicated), m_mgr->get_thread(tid));
}

// ---------------------------------------------------------------------------
// 6. GetCurrentThreadId returns the calling thread's guest TID
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, GetCurrentThreadIdReturnsMainTid) {
    EXPECT_EQ(m_mgr->get_current_thread_id(), 1u);
}

TEST_F(ThreadTest, GetCurrentThreadIdFollowsPerHostThreadTid) {
    ThreadManager::set_current_thread_id(7);
    EXPECT_EQ(m_mgr->get_current_thread_id(), 7u);
    EXPECT_EQ(ThreadManager::get_current_thread_id_static(), 7u);
    ThreadManager::set_current_thread_id(1);
}

TEST_F(ThreadTest, MainThreadIsMarkedLaunched) {
    const GuestThread* main_thread = m_mgr->get_thread(1);
    ASSERT_NE(main_thread, nullptr);
    EXPECT_TRUE(main_thread->launched);
}

// ---------------------------------------------------------------------------
// 7. TerminateThread sets exit code and terminated flag
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, TerminateThreadSetsExitCodeAndFlag) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, 0, &tid);

    EXPECT_TRUE(m_mgr->terminate_thread(h, 99));

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_TRUE(gt->terminated);
    EXPECT_EQ(gt->exit_code, 99u);

    // get_exit_code should agree
    auto result = m_mgr->get_exit_code(h);
    ASSERT_FALSE(result.is_err());
    EXPECT_EQ(result.value(), 99u);
}

// Expected values: tools/oracle_probe/terminate_thread_probe{32,64}.expected.
TEST_F(ThreadTest, TerminateThreadTwiceKeepsFirstExitCode) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, 0, &tid);
    ASSERT_TRUE(m_mgr->terminate_thread(h, 0x5A));
    set_last_error(0xDEAD);
    EXPECT_TRUE(m_mgr->terminate_thread(h, 0x5B));  // ALREADY_TERMINATED ret=1
    EXPECT_EQ(m_mgr->get_exit_code(h).value(), 0x5Au);
}

TEST_F(ThreadTest, TerminateThreadAfterNormalExitKeepsExitCode) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, 0, &tid);
    m_mgr->exit_thread(tid, 3);
    EXPECT_TRUE(m_mgr->terminate_thread(h, 0x5C));  // ALREADY_EXITED ret=1
    EXPECT_EQ(m_mgr->get_exit_code(h).value(), 3u);
}

TEST_F(ThreadTest, TerminateThreadRejectsNonThreadHandles) {
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->terminate_thread(NULL_HANDLE, 1));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->terminate_thread(0xFFFFFFFFu, 1));  // process pseudo
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(ThreadTest, TerminateThreadRequiresThreadTerminateAccess) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &tid);
    constexpr u32 kSynchronize = 0x00100000;
    Handle limited = m_mgr->open_thread(
        tid, ThreadManager::THREAD_QUERY_INFORMATION | kSynchronize);
    ASSERT_NE(limited, NULL_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->terminate_thread(limited, 0x59));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_FALSE(m_mgr->get_thread(tid)->terminated);

    Handle term = m_mgr->open_thread(tid, ThreadManager::THREAD_TERMINATE);
    EXPECT_TRUE(m_mgr->terminate_thread(term, 0x5A));
    EXPECT_EQ(m_mgr->get_exit_code(h).value(), 0x5Au);
}

TEST_F(ThreadTest, ResumeAfterTerminatingSuspendedThreadReturnsZero) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &tid);
    ASSERT_TRUE(m_mgr->terminate_thread(h, 0x57));
    EXPECT_EQ(m_mgr->resume_thread(h), 0u);  // SUSPENDED resume=0x0
    EXPECT_EQ(m_mgr->get_exit_code(h).value(), 0x57u);
}

// ---------------------------------------------------------------------------
// 8. Null start address returns INVALID_HANDLE
// ---------------------------------------------------------------------------
// Learn CreateThread: "may succeed even if lpStartAddress points to data,
// code, or is not accessible"; the thread then faults when it runs
// (thread_lifecycle_probe CREATE_NULL_START ok=1 err=dead).
TEST_F(ThreadTest, NullStartAddressStillCreatesTheThread) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0, 0, CREATE_SUSPENDED, &tid);
    EXPECT_NE(h, INVALID_HANDLE);
    EXPECT_NE(h, NULL_HANDLE);
    EXPECT_NE(tid, 0u);
}

// Windows builds the stack/TEB inside CreateThread; when that fails the call
// fails with ERROR_NOT_ENOUGH_MEMORY and no thread object exists.
TEST_F(ThreadTest, ContextAllocationFailureFailsCreateThread) {
    m_mgr->set_thread_context_callbacks(
        [](GuestThread&) { return false; }, [](GuestThread&) {});
    set_last_error(0xDEAD);
    u32 tid = 0xFFFF;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, 0, &tid);
    EXPECT_EQ(h, NULL_HANDLE);
    EXPECT_EQ(get_last_error(), 8u);  // ERROR_NOT_ENOUGH_MEMORY
    EXPECT_EQ(tid, 0xFFFFu);
}

// thread_lifecycle_probe SUSPENDED_START: x64 start context is
// RIP = RtlUserThreadStart, RCX = start, RDX = parameter, other GPRs 0,
// EFLAGS 0x200, RSP = 8 mod 16; x86 EIP = RtlUserThreadStart, EAX = start,
// EBX = parameter, EFLAGS 0x202.
TEST_F(ThreadTest, SeededStartContextMatchesWindows) {
    m_mgr->set_start_thunks(0x7FFB00001000ull, 0x7FFB00002000ull);
    cpu::CpuState c64;
    c64.init_long_mode();
    c64.set_sp(0x10000000);
    c64.gpr.set64(static_cast<u8>(cpu::RegIndex::R8), 0x55);
    m_mgr->seed_start_context(c64, 0x401000, 0x1234);
    EXPECT_EQ(c64.rip, 0x7FFB00001000ull);
    EXPECT_EQ(c64.gpr.get64(static_cast<u8>(cpu::RegIndex::RCX)), 0x401000u);
    EXPECT_EQ(c64.gpr.get64(static_cast<u8>(cpu::RegIndex::RDX)), 0x1234u);
    EXPECT_EQ(c64.gpr.get64(static_cast<u8>(cpu::RegIndex::R8)), 0u);
    EXPECT_EQ(c64.sp() & 0xF, 8u);
    EXPECT_LT(c64.sp(), 0x10000000u);
    EXPECT_EQ(c64.flags.rflags, 0x200u);
    cpu::CpuState c32;
    c32.init_protected_mode();
    c32.set_sp(0x00200000);
    m_mgr->seed_start_context(c32, 0x401000, 0x1234);
    EXPECT_EQ(c32.rip, 0x7FFB00001000ull);
    EXPECT_EQ(c32.gpr.get32(static_cast<u8>(cpu::RegIndex::RAX)), 0x401000u);
    EXPECT_EQ(c32.gpr.get32(static_cast<u8>(cpu::RegIndex::RBX)), 0x1234u);
    EXPECT_EQ(c32.flags.rflags, 0x202u);
}

// ---------------------------------------------------------------------------
// 9. Default stack size is 1 MB when caller passes 0
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, DefaultStackSizeIsOneMB) {
    u32 tid = 0;
    m_mgr->create_thread(0, 0x1000, 0, 0, &tid);

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_EQ(gt->stack_size, static_cast<usize>(1024 * 1024));
}

// ---------------------------------------------------------------------------
// 10. ResumeThread on suspended thread clears suspended flag
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, ResumeThreadClearsSuspendedFlag) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &tid);

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_GT(gt->suspend_count, 0u);

    u32 prev = m_mgr->resume_thread(h);
    EXPECT_EQ(prev, 1u);          // was suspended → previous count = 1
    EXPECT_EQ(gt->suspend_count, 0u);   // now running
}

// ---------------------------------------------------------------------------
// 11. ExitThread sets the exit code
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, ExitThreadSetsExitCode) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x2000, 0, 0, &tid);

    m_mgr->exit_thread(tid, 42);

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_TRUE(gt->terminated);
    EXPECT_EQ(gt->exit_code, 42u);

    auto result = m_mgr->get_exit_code(h);
    ASSERT_FALSE(result.is_err());
    EXPECT_EQ(result.value(), 42u);
}

TEST_F(ThreadTest, ExitThreadNoreturnTransfersToCallbackSentinel) {
    u32 tid = 0;
    (void)m_mgr->create_thread(0, 0x2000, 0, 0, &tid);
    ThreadManager::set_current_thread_id(tid);

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Protected;
    cpu.rip = 0x8841A3; // MSVC int3 after ExitThread in Agent
    cpu.thunk_control_transfer = false;

    // Minimal marshaler attached to the CPU; args unused by noreturn helper.
    GuestMemory memory;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    m_mgr->exit_current_thread_noreturn(m, 7);

    // Learn ExitThread: DLL_THREAD_DETACH runs before the thread object is
    // signaled (thread_lifecycle_probe detach_before_signal=1), so the
    // runner -- not ExitThread itself -- marks it terminated afterwards.
    EXPECT_FALSE(m_mgr->is_thread_terminated(tid));
    EXPECT_FALSE(m_mgr->primary_exit_pending());
    EXPECT_TRUE(cpu.thunk_control_transfer);
    // A 32-bit guest only sees the low dword of the callback sentinel, and the
    // execution loop matches on exactly that value. Zero would neither match nor
    // be executable, so the thread would fault at RIP 0 instead of unwinding.
    EXPECT_EQ(cpu.rip, callback_return_sentinel(/*is_64bit=*/false));
    EXPECT_NE(cpu.rip, 0u);
    EXPECT_EQ(cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::RAX)), 7u);
}

// Learn ExitThread: the process ends only when the LAST thread exits.
// thread_lifecycle_probe.expected: after the primary thread's ExitThread(5) a
// worker keeps running, sees the primary signaled with code 5, and the
// process exit code is the worker's 42.
TEST_F(ThreadTest, PrimaryExitThreadWithLiveWorkerIsNotProcessExit) {
    u32 worker = 0;
    (void)m_mgr->create_thread(0, 0x2000, 0, CREATE_SUSPENDED, &worker);
    ThreadManager::set_current_thread_id(m_mgr->main_thread_id());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    GuestMemory memory;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    m_mgr->exit_current_thread_noreturn(m, 5);
    EXPECT_TRUE(m_mgr->primary_exit_pending());
    EXPECT_EQ(m_mgr->primary_exit_code(), 5u);
    EXPECT_EQ(cpu.rip, process_return_sentinel(true));
    EXPECT_TRUE(m_mgr->any_live_thread_except(m_mgr->main_thread_id()));
    m_mgr->exit_thread(m_mgr->main_thread_id(), 5);
    m_mgr->exit_thread(worker, 42);
    EXPECT_FALSE(m_mgr->any_live_thread_except(m_mgr->main_thread_id()));
    EXPECT_EQ(m_mgr->last_exit_code(), 42u);
}

TEST_F(ThreadTest, PrimaryExitThreadAsLastThreadIsProcessExit) {
    ThreadManager::set_current_thread_id(m_mgr->main_thread_id());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    GuestMemory memory;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    m_mgr->exit_current_thread_noreturn(m, 9);
    EXPECT_FALSE(m_mgr->primary_exit_pending());
    EXPECT_EQ(cpu.rip, process_return_sentinel(true));
    EXPECT_EQ(cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RAX)), 9u);
}

// ---------------------------------------------------------------------------
// 12. SuspendThread on a running thread sets suspended flag
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, SuspendThreadSetsSuspendedFlag) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, 0, &tid);

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_EQ(gt->suspend_count, 0u);

    u32 prev = m_mgr->suspend_thread(h);
    EXPECT_EQ(prev, 0u);         // was running → previous count = 0
    EXPECT_GT(gt->suspend_count, 0u);
}

// Windows caps the suspend count at MAXIMUM_SUSPEND_COUNT and refuses to
// suspend a thread that has already exited. A caller that suspends in a loop
// relies on those failures to stop; reporting success forever hangs it.
TEST_F(ThreadTest, SuspendThreadStopsAtMaximumSuspendCount) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, 0, &tid);

    for (u32 i = 0; i < ThreadManager::MAXIMUM_SUSPEND_COUNT; ++i) {
        auto status = ThreadManager::SuspendResult::CountExceeded;
        EXPECT_EQ(m_mgr->suspend_thread(h, &status), i);
        EXPECT_EQ(status, ThreadManager::SuspendResult::Ok);
    }

    auto status = ThreadManager::SuspendResult::Ok;
    EXPECT_EQ(m_mgr->suspend_thread(h, &status), ~0u);
    EXPECT_EQ(status, ThreadManager::SuspendResult::CountExceeded);

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_EQ(gt->suspend_count, ThreadManager::MAXIMUM_SUSPEND_COUNT);
}

TEST_F(ThreadTest, SuspendThreadFailsOnAnExitedThread) {
    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, 0, &tid);
    ASSERT_TRUE(m_mgr->terminate_thread(h, 0));

    auto status = ThreadManager::SuspendResult::Ok;
    EXPECT_EQ(m_mgr->suspend_thread(h, &status), ~0u);
    EXPECT_EQ(status, ThreadManager::SuspendResult::Terminating);

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_EQ(gt->suspend_count, 0u);
}

// Suspend/Resume failure codes and the pseudo handle, all measured by
// tools/oracle_probe/thread_lifecycle_probe.expected (SUSPEND_* / RESUME_*).
TEST_F(ThreadTest, SuspendResumeErrorsMatchWindows) {
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->suspend_thread(0x7FF0), ~0u);
    EXPECT_EQ(get_last_error(), 6u);  // ERROR_INVALID_HANDLE
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->resume_thread(0x7FF0), ~0u);
    EXPECT_EQ(get_last_error(), 6u);

    u32 tid = 0;
    Handle h = m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &tid);
    for (int i = 1; i < 127; ++i) ASSERT_EQ(m_mgr->suspend_thread(h), u32(i));
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->suspend_thread(h), ~0u);
    EXPECT_EQ(get_last_error(), 156u);  // ERROR_SIGNAL_REFUSED
    EXPECT_EQ(m_mgr->resume_thread(h), 127u);

    // A handle without THREAD_SUSPEND_RESUME.
    Handle limited = m_handles->allocate(HandleType::Thread,
                                         m_mgr->get_thread(tid), 0,
                                         ThreadManager::THREAD_QUERY_INFORMATION);
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->suspend_thread(limited), ~0u);
    EXPECT_EQ(get_last_error(), 5u);  // ERROR_ACCESS_DENIED
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->resume_thread(limited), ~0u);
    EXPECT_EQ(get_last_error(), 5u);

    ASSERT_TRUE(m_mgr->terminate_thread(h, 0));
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->suspend_thread(h), ~0u);
    EXPECT_EQ(get_last_error(), 5u);  // SUSPEND_EXITED
    set_last_error(0xDEAD);
    EXPECT_NE(m_mgr->resume_thread(h), ~0u);  // RESUME_EXITED succeeds
    EXPECT_EQ(get_last_error(), 0xDEADu);

    // GetCurrentThread() names the caller: not suspended -> 0.
    ThreadManager::set_current_thread_id(m_mgr->main_thread_id());
    EXPECT_EQ(m_mgr->resume_thread(ThreadManager::get_current_thread()), 0u);
}

// thread_lifecycle_probe SETPRIO / BG_* / GETPRIO_*.
TEST_F(ThreadTest, ThreadPriorityValidatedAndStored) {
    // The levels below also reach this host thread's QoS class; leave the
    // runner's scheduling as it was for the tests after this one.
    const qos_class_t runner_qos = qos_class_self();
    struct RestoreQos {
        qos_class_t qos;
        ~RestoreQos() { (void)pthread_set_qos_class_self_np(qos, 0); }
    } restore{runner_qos};
    ThreadManager::set_current_thread_id(m_mgr->main_thread_id());
    const Handle cur = ThreadManager::get_current_thread();
    EXPECT_EQ(m_mgr->set_thread_priority(cur, 2), 0u);
    EXPECT_EQ(m_mgr->get_thread_priority(cur), 2);
    for (i32 bad : {7, 100, -7, 3, -3, 6, -6}) {
        EXPECT_EQ(m_mgr->set_thread_priority(cur, bad), 87u) << bad;
        EXPECT_EQ(m_mgr->get_thread_priority(cur), 2);
    }
    EXPECT_EQ(m_mgr->set_thread_priority(cur, 15), 0u);
    EXPECT_EQ(m_mgr->set_thread_priority(cur, -15), 0u);
    EXPECT_EQ(m_mgr->get_thread_priority(cur), -15);
    EXPECT_EQ(m_mgr->set_thread_priority(0x7FF0, 0), 6u);
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->get_thread_priority(0x7FF0), 0x7FFFFFFF);
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(m_mgr->set_thread_priority(cur, 0x10000), 0u);
    EXPECT_EQ(m_mgr->set_thread_priority(cur, 0x10000), 400u);
    EXPECT_EQ(m_mgr->get_thread_priority(cur), -4);
    EXPECT_EQ(m_mgr->set_thread_priority(cur, 0x20000), 0u);
    EXPECT_EQ(m_mgr->set_thread_priority(cur, 0x20000), 401u);
    u32 tid = 0;
    Handle other = m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &tid);
    EXPECT_EQ(m_mgr->set_thread_priority(other, 0x10000), 87u);
    EXPECT_EQ(m_mgr->set_thread_priority(other, -2), 0u);
    EXPECT_EQ(m_mgr->get_thread_priority(other), -2);
    Handle limited = m_handles->allocate(HandleType::Thread,
                                         m_mgr->get_thread(tid), 0,
                                         ThreadManager::THREAD_QUERY_INFORMATION);
    EXPECT_EQ(m_mgr->set_thread_priority(limited, 1), 5u);
    EXPECT_EQ(m_mgr->get_thread_priority(limited), -2);
}

// SetThreadPriority is scheduling, not a stored number. Learn "Scheduling
// Priorities" fixes the base priority (process class x thread level); the host
// can only express it as a QoS class, mapped in thread.cpp. A level a thread
// sets on itself applies at once; one set on another thread applies at that
// thread's next wait.
TEST_F(ThreadTest, ThreadPriorityBecomesHostQosClass) {
    const qos_class_t runner_qos = qos_class_self();
    struct RestoreQos {
        qos_class_t qos;
        ~RestoreQos() {
            (void)pthread_set_qos_class_self_np(qos, 0);
            ThreadManager::set_qos_floor_for_test(std::nullopt);
        }
    } restore{runner_qos};
    // The unraised map; the VITREO_THREAD_QOS_FLOOR host policy that lifts
    // base 2..7 has its own test (QosFloorArmSwitchReappliesOnEachThread).
    ThreadManager::set_qos_floor_for_test(HostQosFloor::Utility);
    ThreadManager::set_current_thread_id(m_mgr->main_thread_id());
    const Handle cur = ThreadManager::get_current_thread();
    const struct { i32 level; qos_class_t qos; } levels[] = {
        {1, QOS_CLASS_USER_INTERACTIVE},    // ABOVE_NORMAL: base 9
        {0, QOS_CLASS_USER_INITIATED},      // NORMAL: 8
        {-1, QOS_CLASS_DEFAULT},            // BELOW_NORMAL: 7
        {-2, QOS_CLASS_UTILITY},            // LOWEST: 6
        {-15, QOS_CLASS_BACKGROUND},        // IDLE: 1
        {2, QOS_CLASS_USER_INTERACTIVE},    // HIGHEST: 10
        {15, QOS_CLASS_USER_INTERACTIVE},   // TIME_CRITICAL: 15
    };
    for (const auto& c : levels) {
        ASSERT_EQ(m_mgr->set_thread_priority(cur, c.level), 0u) << c.level;
        EXPECT_EQ(qos_class_self(), c.qos) << c.level;
    }
    // THREAD_MODE_BACKGROUND_BEGIN wins over the level; END restores it.
    ASSERT_EQ(m_mgr->set_thread_priority(cur, 0x10000), 0u);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_BACKGROUND);
    ASSERT_EQ(m_mgr->set_thread_priority(cur, 0x20000), 0u);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_USER_INTERACTIVE);
    // The process class moves the base: NORMAL level in IDLE_PRIORITY_CLASS
    // is 4, in ABOVE_NORMAL_PRIORITY_CLASS 10.
    ASSERT_EQ(m_mgr->set_thread_priority(cur, 0), 0u);
    m_mgr->set_process_scheduling(0x40u, false);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_UTILITY);
    m_mgr->set_process_scheduling(0x8000u, false);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_USER_INTERACTIVE);
    m_mgr->set_process_scheduling(0x20u, true);   // PROCESS_MODE_BACKGROUND
    EXPECT_EQ(qos_class_self(), QOS_CLASS_BACKGROUND);
    m_mgr->set_process_scheduling(0x20u, false);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_USER_INITIATED);

    // Another thread's level, set while it runs, reaches it at its next wait
    // (the wait APIs call refresh_current_thread_scheduling on entry).
    u32 tid = 0;
    const Handle other = m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED,
                                              &tid);
    ASSERT_NE(other, 0u);
    std::atomic<int> stage{0};
    qos_class_t started = QOS_CLASS_UNSPECIFIED;
    qos_class_t before_wait = QOS_CLASS_UNSPECIFIED;
    qos_class_t after_wait = QOS_CLASS_UNSPECIFIED;
    std::thread host([&] {
        ThreadManager::set_current_thread_id(tid);
        m_mgr->apply_current_thread_scheduling();
        started = qos_class_self();
        stage.store(1);
        while (stage.load() != 2) std::this_thread::yield();
        before_wait = qos_class_self();
        ThreadManager::refresh_current_thread_scheduling();
        after_wait = qos_class_self();
        (void)pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0);
    });
    while (stage.load() != 1) std::this_thread::yield();
    ASSERT_EQ(m_mgr->set_thread_priority(other, -2), 0u);
    stage.store(2);
    host.join();
    EXPECT_EQ(started, QOS_CLASS_USER_INITIATED);   // NORMAL at creation
    EXPECT_EQ(before_wait, QOS_CLASS_USER_INITIATED);
    EXPECT_EQ(after_wait, QOS_CLASS_UTILITY);       // LOWEST
}

// The host policy table (host_qos_class_for). Expected values are the policy
// definition: base >= 9 USER_INTERACTIVE, 8 USER_INITIATED, 7 DEFAULT, 2..6
// UTILITY, <= 1 or background mode BACKGROUND; VITREO_THREAD_QOS_FLOOR raises
// only base 2..7; explicit EcoQoS is UTILITY and explicit HighQoS at least
// USER_INITIATED, neither overriding background mode or base <= 1.
TEST(HostQosPolicy, MappingTableWithAndWithoutFloor) {
    constexpr unsigned BG = QOS_CLASS_BACKGROUND, UT = QOS_CLASS_UTILITY,
                       DF = QOS_CLASS_DEFAULT, UIN = QOS_CLASS_USER_INITIATED,
                       UIA = QOS_CLASS_USER_INTERACTIVE;
    const auto sys = PowerThrottlingQos::SystemManaged;
    // base: 0..16 under each floor.
    const unsigned no_floor[] = {BG, BG, UT, UT, UT, UT, UT, DF, UIN,
                                 UIA, UIA, UIA, UIA, UIA, UIA, UIA, UIA};
    const unsigned default_floor[] = {BG, BG, DF, DF, DF, DF, DF, DF, UIN,
                                      UIA, UIA, UIA, UIA, UIA, UIA, UIA, UIA};
    const unsigned ui_floor[] = {BG, BG, UIN, UIN, UIN, UIN, UIN, UIN, UIN,
                                 UIA, UIA, UIA, UIA, UIA, UIA, UIA, UIA};
    for (int base = 0; base <= 16; ++base) {
        EXPECT_EQ(host_qos_class_for(base, false, sys, HostQosFloor::Utility),
                  no_floor[base]) << base;
        EXPECT_EQ(host_qos_class_for(base, false, sys, HostQosFloor::Default),
                  default_floor[base]) << base;
        EXPECT_EQ(host_qos_class_for(base, false, sys,
                                     HostQosFloor::UserInitiated),
                  ui_floor[base]) << base;
        for (HostQosFloor f : {HostQosFloor::Utility, HostQosFloor::Default,
                               HostQosFloor::UserInitiated}) {
            // Background mode, and explicit EcoQoS, keep their low class
            // under any floor; HighQoS lifts to USER_INITIATED.
            EXPECT_EQ(host_qos_class_for(base, true, sys, f), BG) << base;
            EXPECT_EQ(host_qos_class_for(base, true, PowerThrottlingQos::High, f),
                      BG) << base;
            EXPECT_EQ(host_qos_class_for(base, false, PowerThrottlingQos::Eco, f),
                      base <= 1 ? BG : UT) << base;
            const unsigned high =
                host_qos_class_for(base, false, PowerThrottlingQos::High, f);
            EXPECT_EQ(high, base <= 1 ? BG : base >= 9 ? UIA : UIN) << base;
        }
    }
    // Learn "Scheduling Priorities": the bases the table is keyed on.
    EXPECT_EQ(windows_base_priority(0x20u, -1), 7);    // NORMAL, BELOW_NORMAL
    EXPECT_EQ(windows_base_priority(0x20u, -2), 6);    // NORMAL, LOWEST
    EXPECT_EQ(windows_base_priority(0x4000u, 0), 6);   // BELOW_NORMAL class
    EXPECT_EQ(windows_base_priority(0x40u, -2), 2);    // IDLE class, LOWEST
    EXPECT_EQ(windows_base_priority(0x40u, -15), 1);   // THREAD_PRIORITY_IDLE
}

// tools/oracle_probe/power_throttling_probe.expected (Windows 10.0.26200,
// x86 and x64 identical).
TEST(HostQosPolicy, PowerThrottlingValidationMatchesOracle) {
    constexpr u32 kInvalid = 87, kBadLength = 24;
    struct Case { u32 size, version, control, state, error; };
    const Case thread_cases[] = {
        {12, 1, 1, 1, 0},  {12, 1, 1, 0, 0},  {12, 1, 0, 0, 0},
        {12, 1, 0, 1, kInvalid}, {12, 1, 2, 0, kInvalid},
        {12, 1, 1, 2, kInvalid}, {12, 0, 1, 1, 0}, {12, 2, 1, 1, kInvalid},
        {12, 3, 1, 1, kInvalid}, {8, 1, 1, 1, kInvalid}, {0, 1, 1, 1, kInvalid},
        {11, 1, 1, 1, kInvalid}, {13, 1, 1, 1, 0}, {16, 1, 1, 1, 0},
        {32, 1, 1, 1, 0}, {12, 0, 0, 1, kInvalid}, {12, 0, 2, 0, kInvalid},
        {12, 1, 0xFFFFFFFFu, 0, kInvalid}, {12, 0, 1, 0, 0},
    };
    for (const Case& c : thread_cases) {
        EXPECT_EQ(validate_thread_power_throttling(c.size, c.version, c.control,
                                                   c.state), c.error)
            << c.size << " v" << c.version << " c" << c.control << " s" << c.state;
    }
    const Case process_cases[] = {
        {12, 1, 1, 1, 0}, {12, 1, 4, 4, 0}, {12, 1, 1, 0, 0}, {12, 1, 0, 0, 0},
        {12, 1, 5, 5, 0}, {12, 1, 5, 1, 0}, {12, 1, 0, 1, kInvalid},
        {12, 1, 2, 0, 0}, {12, 1, 1, 2, kInvalid}, {12, 0, 1, 1, kInvalid},
        {12, 2, 1, 1, kInvalid}, {8, 1, 1, 1, kBadLength},
        {16, 1, 1, 1, kBadLength}, {13, 1, 1, 1, kBadLength},
        {32, 1, 1, 1, kBadLength}, {12, 0, 1, 0, kInvalid},
        {12, 1, 8, 0, kInvalid}, {12, 1, 0xFFFFFFFFu, 0, kInvalid},
        {12, 1, 2, 2, 0}, {12, 1, 0xFFFFFFFFu, 0xFFFFFFFFu, kInvalid},
        {12, 1, 7, 5, 0}, {8, 0, 1, 1, kBadLength},
    };
    for (const Case& c : process_cases) {
        EXPECT_EQ(validate_process_power_throttling(c.size, c.version, c.control,
                                                    c.state), c.error)
            << c.size << " v" << c.version << " c" << c.control << " s" << c.state;
    }
}

// SetThreadInformation(ThreadPowerThrottling) / SetProcessInformation
// (ProcessPowerThrottling) reach the host QoS class; rights per the probe.
TEST_F(ThreadTest, PowerThrottlingBecomesHostQosClass) {
    const qos_class_t runner_qos = qos_class_self();
    struct Restore {
        qos_class_t qos;
        ~Restore() {
            (void)pthread_set_qos_class_self_np(qos, 0);
            ThreadManager::set_qos_floor_for_test(std::nullopt);
        }
    } restore{runner_qos};
    ThreadManager::set_qos_floor_for_test(HostQosFloor::Utility);
    ThreadManager::set_current_thread_id(m_mgr->main_thread_id());
    const Handle cur = ThreadManager::get_current_thread();
    ASSERT_EQ(m_mgr->set_thread_priority(cur, -2), 0u);   // base 6
    EXPECT_EQ(qos_class_self(), QOS_CLASS_UTILITY);
    ASSERT_EQ(m_mgr->set_thread_power_throttling(cur, 1, 0), 0u);  // HighQoS
    EXPECT_EQ(qos_class_self(), QOS_CLASS_USER_INITIATED);
    ASSERT_EQ(m_mgr->set_thread_power_throttling(cur, 0, 0), 0u);  // system
    EXPECT_EQ(qos_class_self(), QOS_CLASS_UTILITY);
    ASSERT_EQ(m_mgr->set_thread_priority(cur, 1), 0u);    // base 9
    EXPECT_EQ(qos_class_self(), QOS_CLASS_USER_INTERACTIVE);
    ASSERT_EQ(m_mgr->set_thread_power_throttling(cur, 1, 1), 0u);  // EcoQoS
    EXPECT_EQ(qos_class_self(), QOS_CLASS_UTILITY);
    // The thread's own control wins over the process's; system-managed
    // threads follow the process.
    m_mgr->set_process_power_throttling(1, 0);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_UTILITY);
    ASSERT_EQ(m_mgr->set_thread_power_throttling(cur, 0, 0), 0u);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_USER_INTERACTIVE);
    ASSERT_EQ(m_mgr->set_thread_priority(cur, 0), 0u);
    m_mgr->set_process_power_throttling(1, 1);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_UTILITY);
    m_mgr->set_process_power_throttling(0, 0);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_USER_INITIATED);

    u32 tid = 0;
    const Handle other = m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &tid);
    ASSERT_NE(other, 0u);
    EXPECT_EQ(m_mgr->set_thread_power_throttling(other, 1, 1), 0u);
    EXPECT_EQ(m_mgr->set_thread_power_throttling(0x7FF0, 1, 1), 6u);
    const Handle limited = m_handles->allocate(
        HandleType::Thread, m_mgr->get_thread(tid), 0,
        ThreadManager::THREAD_SET_LIMITED_INFORMATION);
    EXPECT_EQ(m_mgr->set_thread_power_throttling(limited, 1, 1), 5u);
    EXPECT_EQ(m_mgr->set_thread_memory_priority(limited, 2), 5u);
    const Handle query = m_handles->allocate(
        HandleType::Thread, m_mgr->get_thread(tid), 0,
        ThreadManager::THREAD_QUERY_INFORMATION);
    EXPECT_EQ(m_mgr->set_thread_power_throttling(query, 1, 1), 5u);
    const Handle set_info = m_handles->allocate(
        HandleType::Thread, m_mgr->get_thread(tid), 0,
        ThreadManager::THREAD_SET_INFORMATION);
    EXPECT_EQ(m_mgr->set_thread_power_throttling(set_info, 1, 1), 0u);
    EXPECT_EQ(m_mgr->set_thread_memory_priority(set_info, 2), 0u);
}

// The exports end to end, x86 and x64: power_throttling_probe.expected.
class PowerThrottlingApiTest : public ::testing::TestWithParam<bool> {
protected:
    GuestMemory memory;
    HandleTable handles;
    ThreadManager threads{handles};
    ProcessManager process;
    capsule::Vfs vfs;
    Kernel32ExtManager manager{memory, vfs};
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    static constexpr GuestAddr info = 0x4000;
    qos_class_t runner_qos = qos_class_self();

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x1000, 0x8000, MemoryProtection::ReadWrite).is_ok());
        if (GetParam()) cpu.init_long_mode();
        else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x3000;
        marshaler.attach_cpu(cpu);
        manager.set_handle_table(&handles);
        manager.set_thread_manager(&threads);
        manager.set_process_manager(&process);
        ThreadManager::set_current_thread_id(threads.main_thread_id());
    }
    void TearDown() override { (void)pthread_set_qos_class_self_np(runner_qos, 0); }
    u64 self_thread() const { return GetParam() ? ~u64{1} : 0xFFFFFFFEu; }
    u64 self_process() const { return GetParam() ? ~u64{0} : 0xFFFFFFFFu; }
    void put(u32 a, u32 b, u32 c) {
        ASSERT_TRUE(memory.write_u32(info, a).is_ok());
        ASSERT_TRUE(memory.write_u32(info + 4, b).is_ok());
        ASSERT_TRUE(memory.write_u32(info + 8, c).is_ok());
    }
    // Returns the BOOL; @p error gets GetLastError when it is FALSE.
    u32 call(bool thread_api, u64 handle, u32 cls, GuestAddr buf, u32 size,
             u32& error) {
        cpu.pending_raise.clear();
        if (GetParam()) {
            cpu.gpr.rcx = handle; cpu.gpr.rdx = cls; cpu.gpr.r8 = buf; cpu.gpr.r9 = size;
        } else {
            EXPECT_TRUE(memory.write_u32(0x3004, static_cast<u32>(handle)).is_ok());
            EXPECT_TRUE(memory.write_u32(0x3008, cls).is_ok());
            EXPECT_TRUE(memory.write_u32(0x300C, static_cast<u32>(buf)).is_ok());
            EXPECT_TRUE(memory.write_u32(0x3010, size).is_ok());
        }
        set_last_error(0xDEAD);
        if (thread_api) manager.impl_SetThreadInformation(marshaler);
        else manager.impl_SetProcessInformation(marshaler);
        const u32 ok = static_cast<u32>(cpu.gpr.rax);
        error = ok != 0 ? 0 : get_last_error();
        return ok;
    }
};

TEST_P(PowerThrottlingApiTest, SetThreadInformation) {
    u32 e = 0;
    put(1, 1, 1);
    EXPECT_EQ(call(true, self_thread(), 3, info, 12, e), 1u);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_UTILITY);          // EcoQoS
    put(1, 1, 0);
    EXPECT_EQ(call(true, self_thread(), 3, info, 16, e), 1u);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_USER_INITIATED);   // HighQoS
    put(0, 0, 0);
    EXPECT_EQ(call(true, self_thread(), 3, info, 12, e), 1u);
    put(1, 0, 1);
    EXPECT_EQ(call(true, self_thread(), 3, info, 12, e), 0u); EXPECT_EQ(e, 87u);
    put(2, 1, 1);
    EXPECT_EQ(call(true, self_thread(), 3, info, 12, e), 0u); EXPECT_EQ(e, 87u);
    EXPECT_EQ(call(true, self_thread(), 3, info, 11, e), 0u); EXPECT_EQ(e, 87u);
    put(1, 1, 1);
    EXPECT_EQ(call(true, 0x1234, 3, info, 12, e), 0u); EXPECT_EQ(e, 6u);
    EXPECT_EQ(call(true, 0, 3, info, 12, e), 0u); EXPECT_EQ(e, 6u);
    put(1, 2, 0);   // parameters are checked before the handle
    EXPECT_EQ(call(true, 0x1234, 3, info, 12, e), 0u); EXPECT_EQ(e, 87u);
    // ThreadMemoryPriority: MEMORY_PRIORITY_INFORMATION, 1..5.
    put(5, 0, 0);
    EXPECT_EQ(call(true, self_thread(), 0, info, 4, e), 1u);
    EXPECT_EQ(call(true, self_thread(), 0, info, 8, e), 0u); EXPECT_EQ(e, 24u);
    EXPECT_EQ(call(true, self_thread(), 0, info, 12, e), 0u); EXPECT_EQ(e, 24u);
    put(6, 0, 0);
    EXPECT_EQ(call(true, self_thread(), 0, info, 4, e), 0u); EXPECT_EQ(e, 87u);
    put(0, 0, 0);
    EXPECT_EQ(call(true, self_thread(), 0, info, 4, e), 0u); EXPECT_EQ(e, 87u);
    // Other classes: bad length for 1/2 at size 12, unknown -> 87.
    put(1, 1, 1);
    EXPECT_EQ(call(true, self_thread(), 1, info, 12, e), 0u); EXPECT_EQ(e, 24u);
    EXPECT_EQ(call(true, self_thread(), 2, info, 8, e), 0u); EXPECT_EQ(e, 24u);
    EXPECT_EQ(call(true, self_thread(), 4, info, 12, e), 0u); EXPECT_EQ(e, 87u);
    EXPECT_EQ(call(true, self_thread(), 99, info, 12, e), 0u); EXPECT_EQ(e, 87u);
    // Unreadable structure: the access violation Windows takes.
    call(true, self_thread(), 3, 0, 12, e);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
}

TEST_P(PowerThrottlingApiTest, SetProcessInformation) {
    u32 e = 0;
    put(1, 1, 1);
    EXPECT_EQ(call(false, self_process(), 4, info, 12, e), 1u);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_UTILITY);          // process EcoQoS
    put(1, 4, 4);
    EXPECT_EQ(call(false, self_process(), 4, info, 12, e), 1u);
    EXPECT_EQ(qos_class_self(), QOS_CLASS_USER_INITIATED);   // speed released
    put(1, 2, 0);
    EXPECT_EQ(call(false, self_process(), 4, info, 12, e), 1u);
    put(1, 8, 0);
    EXPECT_EQ(call(false, self_process(), 4, info, 12, e), 0u); EXPECT_EQ(e, 87u);
    put(0, 1, 1);
    EXPECT_EQ(call(false, self_process(), 4, info, 12, e), 0u); EXPECT_EQ(e, 87u);
    put(1, 1, 1);
    EXPECT_EQ(call(false, self_process(), 4, info, 16, e), 0u); EXPECT_EQ(e, 24u);
    EXPECT_EQ(call(false, 0x1234, 4, info, 8, e), 0u); EXPECT_EQ(e, 24u);
    EXPECT_EQ(call(false, 0x1234, 4, info, 12, e), 0u); EXPECT_EQ(e, 6u);
    // ProcessMemoryPriority is recorded and read back.
    put(2, 0, 0);
    EXPECT_EQ(call(false, self_process(), 0, info, 4, e), 1u);
    EXPECT_EQ(process.memory_priority(), 2u);
    put(6, 0, 0);
    EXPECT_EQ(call(false, self_process(), 0, info, 4, e), 0u); EXPECT_EQ(e, 87u);
    EXPECT_EQ(call(false, self_process(), 0, info, 8, e), 0u); EXPECT_EQ(e, 24u);
    EXPECT_EQ(call(false, self_process(), 99, info, 4, e), 0u); EXPECT_EQ(e, 87u);
    put(1, 0, 0);
    EXPECT_EQ(call(false, self_process(), 4, info, 12, e), 1u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, PowerThrottlingApiTest, ::testing::Bool(),
                         [](const auto& p) { return p.param ? "x64" : "x86"; });

// The "qos-floor" A/B arm: a policy change re-applies on every guest thread at
// its next refresh (the wait entry), on that thread, and only then.
TEST_F(ThreadTest, QosFloorArmSwitchReappliesOnEachThread) {
    struct Restore {
        ~Restore() {
            ThreadManager::set_qos_floor_for_test(std::nullopt);
            diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::None);
        }
    } restore;
    ThreadManager::set_qos_floor_for_test(HostQosFloor::UserInitiated);
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::On);
    u32 tid = 0;
    const Handle other = m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &tid);
    ASSERT_NE(other, 0u);
    ASSERT_EQ(m_mgr->set_thread_priority(other, -2), 0u);  // base 6
    std::atomic<int> stage{0};
    qos_class_t on_start = QOS_CLASS_UNSPECIFIED, off_before = QOS_CLASS_UNSPECIFIED,
                off_after = QOS_CLASS_UNSPECIFIED, on_again = QOS_CLASS_UNSPECIFIED,
                eco_on = QOS_CLASS_UNSPECIFIED;
    auto step = [&](int wait_for) {
        while (stage.load() != wait_for) std::this_thread::yield();
    };
    std::thread host([&] {
        ThreadManager::set_current_thread_id(tid);
        m_mgr->apply_current_thread_scheduling();
        on_start = qos_class_self();
        stage.store(1);
        step(2);
        ThreadManager::refresh_current_thread_scheduling();
        off_before = qos_class_self();   // arm flipped, not yet notified
        stage.store(3);
        step(4);
        ThreadManager::refresh_current_thread_scheduling();
        off_after = qos_class_self();
        stage.store(5);
        step(6);
        ThreadManager::refresh_current_thread_scheduling();
        on_again = qos_class_self();
        stage.store(7);
        step(8);
        ThreadManager::refresh_current_thread_scheduling();
        eco_on = qos_class_self();
        (void)pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0);
    });
    step(1);
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::Off);
    stage.store(2);
    step(3);
    ThreadManager::notify_host_scheduling_policy_changed();
    stage.store(4);
    step(5);
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::On);
    ThreadManager::notify_host_scheduling_policy_changed();
    stage.store(6);
    step(7);
    ASSERT_EQ(m_mgr->set_thread_power_throttling(other, 1, 1), 0u);  // EcoQoS
    stage.store(8);
    host.join();
    EXPECT_EQ(on_start, QOS_CLASS_USER_INITIATED);   // floor on
    EXPECT_EQ(off_before, QOS_CLASS_USER_INITIATED);
    EXPECT_EQ(off_after, QOS_CLASS_UTILITY);         // floor off: plain table
    EXPECT_EQ(on_again, QOS_CLASS_USER_INITIATED);
    EXPECT_EQ(eco_on, QOS_CLASS_UTILITY);            // EcoQoS is exempt
}

// Learn QueueUserAPC: one FIFO; thread_lifecycle_probe APC_KINDS order=1R2
// (user APC, ReadFileEx completion, user APC).
TEST_F(ThreadTest, AlertableDeliveryIsOneFifoAcrossApcKinds) {
    ThreadManager::set_current_thread_id(m_mgr->main_thread_id());
    const u32 tid = m_mgr->main_thread_id();
    ASSERT_TRUE(m_mgr->queue_user_apc(ThreadManager::get_current_thread(), 0x100, 1));
    ASSERT_TRUE(m_mgr->queue_io_completion_apc(0x200, 0, 0, 0));
    ASSERT_TRUE(m_mgr->queue_nt_io_apc(0x300, 0, 0));
    ASSERT_TRUE(m_mgr->queue_user_apc(ThreadManager::get_current_thread(), 0x100, 2));
    std::string order;
    const usize n = m_mgr->drain_all_apcs(
        tid, [&](GuestAddr, u64 p) { order += p == 1 ? '1' : '2'; },
        [&](GuestAddr, u64, u64, u64, u64, u8) { order += 'R'; },
        [&](GuestAddr, GuestAddr, GuestAddr) { order += 'N'; });
    EXPECT_EQ(n, 4u);
    EXPECT_EQ(order, "1RN2");
    // A NULL routine is queued, not rejected (QUEUEAPC_NULL_FN ret=1).
    EXPECT_TRUE(m_mgr->queue_user_apc(ThreadManager::get_current_thread(), 0, 0));
}

// ---------------------------------------------------------------------------
// 13. CPU state is initialised for new thread (RIP, RCX)
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, CpuStateInitialisedCorrectly) {
    u32 tid = 0;
    m_mgr->create_thread(0, 0xAAAA, 0xBBBB, 0, &tid);

    const GuestThread* gt = m_mgr->get_thread(tid);
    ASSERT_NE(gt, nullptr);
    EXPECT_EQ(gt->cpu.rip, 0xAAAAu);
    EXPECT_EQ(gt->cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RCX)), 0xBBBBu);
    EXPECT_TRUE(gt->cpu.is_long_mode());
}

// ---------------------------------------------------------------------------
// 14. thread_id_out == nullptr is safe
// ---------------------------------------------------------------------------
TEST_F(ThreadTest, NullThreadIdOutIsSafe) {
    Handle h = m_mgr->create_thread(0, 0x1000, 0, 0, nullptr);
    EXPECT_NE(h, INVALID_HANDLE);
}

// ---------------------------------------------------------------------------
// 15. Explicit stack size is respected
// ---------------------------------------------------------------------------
// dwStackSize is the COMMIT size unless STACK_SIZE_PARAM_IS_A_RESERVATION;
// defaults come from the EXE header. Every row is a line of
// tools/oracle_probe/thread_lifecycle_probe.expected (EXE reserve 0x280000,
// commit 0x5000): "STACK <name> ... reserve=<r> limit_below_base=<commit>".
TEST(ThreadStackSizes, MatchesWindowsRounding) {
    struct Row { u64 size; u32 flags; u64 reserve; u64 commit; };
    const Row rows[] = {
        {0, 0, 0x280000, 0x5000},                                  // 0
        {0x10000, 0, 0x280000, 0x10000},                           // commit_64k
        {0x100000, 0, 0x280000, 0x100000},                         // commit_1m
        {0x280000, 0, 0x300000, 0x280000},                         // commit_eq_reserve
        {0x300001, 0, 0x400000, 0x301000},                         // commit_3m_plus1
        {1, 0, 0x280000, 0x1000},                                  // commit_1
        {0x10000, STACK_SIZE_PARAM_IS_A_RESERVATION, 0x10000, 0x5000},    // reserve_64k
        {0x900000, STACK_SIZE_PARAM_IS_A_RESERVATION, 0x900000, 0x5000},  // reserve_9m
        {0x123456, STACK_SIZE_PARAM_IS_A_RESERVATION, 0x130000, 0x5000},  // reserve_odd
        {0, STACK_SIZE_PARAM_IS_A_RESERVATION, 0x280000, 0x5000},         // reserve_0
        {1, STACK_SIZE_PARAM_IS_A_RESERVATION, 0x100000, 0x5000},         // reserve_1
    };
    for (const Row& r : rows) {
        const ThreadStackSizes s =
            compute_thread_stack_sizes(r.size, r.flags, 0x280000, 0x5000);
        EXPECT_EQ(s.reserve, r.reserve) << std::hex << r.size << " " << r.flags;
        EXPECT_EQ(s.commit, r.commit) << std::hex << r.size << " " << r.flags;
    }
}

TEST_F(ThreadTest, CreateThreadStackUsesImageDefaultsAndReservationFlag) {
    m_mgr->set_default_stack(0x280000, 0x5000);
    u32 tid = 0;
    (void)m_mgr->create_thread(0, 0x1000, 0, 0, &tid);
    EXPECT_EQ(m_mgr->get_thread(tid)->stack_size, 0x280000u);
    (void)m_mgr->create_thread(0x900000, 0x1000, 0,
                               STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    EXPECT_EQ(m_mgr->get_thread(tid)->stack_size, 0x900000u);
    (void)m_mgr->create_thread(0x300001, 0x1000, 0, 0, &tid);
    EXPECT_EQ(m_mgr->get_thread(tid)->stack_size, 0x400000u);
    EXPECT_EQ(m_mgr->get_thread(tid)->stack_commit, 0x301000u);
}

TEST_F(ThreadTest, TrackedHostWorkerIsWaitedAndJoined) {
    std::atomic<bool> finished{false};
    m_mgr->thread_started();
    std::thread worker([this, &finished] {
        finished.store(true, std::memory_order_release);
        m_mgr->thread_finished();
    });
    m_mgr->track_host_thread(std::move(worker));

    EXPECT_TRUE(
        m_mgr->wait_for_threads(std::chrono::milliseconds(1000)));
    m_mgr->join_host_threads();
    EXPECT_TRUE(finished.load(std::memory_order_acquire));
}

TEST_F(ThreadTest, ReservedPrimaryIdentityIsUsedByThreadState) {
    const u32 reserved = m_mgr->allocate_thread_id();
    ThreadManager child(*m_handles, reserved);
    EXPECT_EQ(child.main_thread_id(), reserved);
    EXPECT_EQ(child.get_current_thread_id(), reserved);
    const GuestThread* primary = child.get_thread(reserved);
    ASSERT_NE(primary, nullptr);
    EXPECT_EQ(primary->thread_id, reserved);
    EXPECT_NE(child.allocate_thread_id(), reserved);
    EXPECT_NE(child.allocate_thread_id(), reserved);
}

TEST_F(ThreadTest, IdentityAllocationFailureDoesNotUseALocalFallback) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "vitreo-thread-id-unusable-scope";
    std::ofstream(root).put('x'); // A file cannot contain the allocator directory.
    const char* scope = std::getenv("VITREO_IPC_SCOPE");
    const std::optional<std::string> previous = scope
        ? std::optional<std::string>(scope) : std::nullopt;
    struct Restore {
        std::optional<std::string> scope;
        fs::path path;
        ~Restore() {
            if (scope) ::setenv("VITREO_IPC_SCOPE", scope->c_str(), 1);
            else ::unsetenv("VITREO_IPC_SCOPE");
            std::error_code ec;
            fs::remove(path, ec);
        }
    } restore{previous, root};
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);
    EXPECT_EQ(m_mgr->allocate_thread_id(), 0u);
    EXPECT_EQ(m_mgr->create_thread(0, 0x1000, 0, 0, nullptr), NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOT_ENOUGH_MEMORY);
    ThreadManager failed(*m_handles);
    EXPECT_EQ(failed.main_thread_id(), 0u);
    EXPECT_EQ(failed.get_thread(0), nullptr);
}

TEST_F(ThreadTest, LocalIdentityExhaustionDoesNotWrapIntoLiveIds) {
    ThreadManager exhausted(*m_handles, UINT32_MAX);
    EXPECT_EQ(exhausted.allocate_thread_id(), 0u);
    EXPECT_EQ(exhausted.allocate_thread_id(), 0u);
    EXPECT_EQ(exhausted.create_thread(0, 0x1000, 0, 0, nullptr), NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOT_ENOUGH_MEMORY);
}

// Native expectations: thread_times_probe{,.x86}.expected; no Vitreo-derived
// values. The host CPU check exercises the real lifecycle binding and cache.
class ThreadTimesContractTest : public ::testing::Test {
protected:
    void check(bool x64) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x5000, MemoryProtection::ReadWrite).is_ok());
        capsule::Vfs vfs;
        HandleTable handles;
        ThreadManager threads(handles);
        Kernel32ExtManager manager(memory, vfs);
        manager.set_handle_table(&handles);
        manager.set_thread_manager(&threads);
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x4000;
        Marshaler marshaler(memory); marshaler.attach_cpu(cpu);
        const u64 pseudo = x64 ? ~u64{1} : static_cast<u32>(-2);
        const std::array<GuestAddr, 4> buffers{0x1000, 0x1010, 0x1020, 0x1030};
        constexpr u64 sentinel = 0xababababababababULL;
        auto query = [&](u64 handle, std::array<GuestAddr, 4> outputs) {
            cpu.pending_raise.clear(); cpu.gpr.rax = 0;
            for (GuestAddr pointer : buffers) EXPECT_TRUE(memory.write_u64(pointer, sentinel).is_ok());
            if (x64) {
                cpu.gpr.rcx = handle; cpu.gpr.rdx = outputs[0];
                cpu.gpr.r8 = outputs[1]; cpu.gpr.r9 = outputs[2];
                EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, outputs[3]).is_ok());
            } else {
                EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, static_cast<u32>(handle)).is_ok());
                for (usize index = 0; index < outputs.size(); ++index)
                    EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 8 + index * sizeof(u32), static_cast<u32>(outputs[index])).is_ok());
            }
            set_last_error(0xdead);
            manager.impl_GetThreadTimes(marshaler);
        };
        auto failed = [&](u64 handle, u32 error) {
            query(handle, buffers);
            EXPECT_EQ(cpu.gpr.rax, 0u); EXPECT_EQ(get_last_error(), error);
            EXPECT_FALSE(cpu.pending_raise.active);
            for (GuestAddr pointer : buffers) EXPECT_EQ(memory.read_u64(pointer).value(), sentinel);
        };
        auto succeeded = [&](u64 handle) {
            query(handle, buffers);
            EXPECT_EQ(cpu.gpr.rax, 1u); EXPECT_EQ(get_last_error(), 0xdeadu);
            EXPECT_FALSE(cpu.pending_raise.active);
            EXPECT_GT(memory.read_u64(buffers[0]).value(), 0u);
        };
        u32 tid = 0;
        const Handle thread = threads.create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &tid);
        ASSERT_NE(thread, INVALID_HANDLE);
        succeeded(thread); succeeded(pseudo);
        failed(0, error_codes::ERROR_INVALID_HANDLE);
        failed(x64 ? ~u64{0} : static_cast<u32>(-1), error_codes::ERROR_INVALID_HANDLE);
        failed(0x12345678, error_codes::ERROR_INVALID_HANDLE);
        for (HandleType type : {HandleType::Event, HandleType::Process}) {
            const Handle wrong = handles.allocate(type, nullptr);
            failed(wrong, error_codes::ERROR_INVALID_HANDLE);
            ASSERT_TRUE(handles.close(wrong));
        }
        const u32 full = ThreadManager::THREAD_QUERY_INFORMATION;
        const u32 limited = ThreadManager::THREAD_QUERY_LIMITED_INFORMATION;
        for (u32 access : {0u, full, limited, sync_access::SYNCHRONIZE,
                           full | sync_access::SYNCHRONIZE, HandleAccess::All}) {
            const Handle copy = threads.duplicate_thread_handle(thread, 0, access);
            ASSERT_NE(copy, INVALID_HANDLE);
            if (access & (full | limited)) succeeded(copy);
            else failed(copy, error_codes::ERROR_ACCESS_DENIED);
            ASSERT_TRUE(handles.close(copy));
        }
        if (x64) {
            failed(static_cast<u64>(thread) | (u64{1} << 32), error_codes::ERROR_INVALID_HANDLE);
            failed(static_cast<u32>(-2), error_codes::ERROR_INVALID_HANDLE);
        }
        for (usize bad = 0; bad < buffers.size(); ++bad) {
            auto outputs = buffers; outputs[bad] = 0;
            query(thread, outputs);
            ASSERT_TRUE(cpu.pending_raise.active);
            EXPECT_EQ(cpu.pending_raise.code, thread_times_oracle::OUTPUT_FAULT);
            EXPECT_EQ(cpu.pending_raise.num_args, 2u);
            EXPECT_EQ(cpu.pending_raise.args[0], 1u); EXPECT_EQ(cpu.pending_raise.args[1], 0u);
            EXPECT_EQ(get_last_error(), 0xdeadu);
            u32 mask = 0;
            for (usize i = 0; i < buffers.size(); ++i)
                if (memory.read_u64(buffers[i]).value() != sentinel) mask |= u32{1} << i;
            EXPECT_EQ(mask, x64 ? thread_times_oracle::WRITE_MASK_64[bad] : thread_times_oracle::WRITE_MASK_32[bad]);
        }
        auto outputs = buffers; outputs[0] = 0;
        query(0, outputs);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
        ASSERT_TRUE(memory.protect_region(0x3000, 0x1000, MemoryProtection::Read).is_ok());
        ASSERT_TRUE(memory.write_u32(0x2ffc, 0xabababab).is_ok());
        outputs = buffers; outputs[0] = 0x2ffc;
        query(thread, outputs);
        ASSERT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.args[1], 0x3000u);
        EXPECT_EQ(memory.read_u32(0x2ffc).value() != 0xabababab,
                  x64 ? thread_times_oracle::EDGE_LOW_WRITTEN_64 : thread_times_oracle::EDGE_LOW_WRITTEN_32);
        outputs = buffers; outputs[0] = 0x3000;
        query(thread, outputs);
        ASSERT_TRUE(cpu.pending_raise.active); EXPECT_EQ(cpu.pending_raise.args[1], 0x3000u);
        outputs.fill(buffers[0]); query(thread, outputs);
        EXPECT_EQ(cpu.gpr.rax, 1u); EXPECT_EQ(memory.read_u64(buffers[0]).value(), 0u);
        outputs = buffers; outputs[0] = 0x1101; query(thread, outputs);
        EXPECT_EQ(cpu.gpr.rax, 1u); EXPECT_FALSE(cpu.pending_raise.active);
        threads.exit_thread(tid, 4); succeeded(thread);
        EXPECT_GE(memory.read_u64(buffers[1]).value(), memory.read_u64(buffers[0]).value());
        ASSERT_TRUE(handles.close(thread)); failed(thread, error_codes::ERROR_INVALID_HANDLE);
    }
};

TEST_F(ThreadTimesContractTest, NativeRightsWidthsOutputsAndLifetime) {
    check(false); check(true);
}

TEST_F(ThreadTest, CpuCountersBelongToExecutingThreadAndSurviveExit) {
    u32 busy_tid = 0, idle_tid = 0;
    const Handle busy = m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &busy_tid);
    const Handle idle = m_mgr->create_thread(0, 0x1000, 0, CREATE_SUSPENDED, &idle_tid);
    ASSERT_NE(busy, INVALID_HANDLE); ASSERT_NE(idle, INVALID_HANDLE);
    std::array<u64, 4> before{}, after{}, retained{}, idle_after{};
    u32 error = 0;
    ASSERT_TRUE(m_mgr->get_thread_times(busy, before, error));
    std::thread worker([&] {
        ThreadManager::set_current_thread_id(busy_tid);
        m_mgr->bind_current_host_thread(busy_tid);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        volatile u64 value = 1;
        do { for (u32 i = 0; i < 10000; ++i) value = value * 1664525 + 1013904223; }
        while (std::chrono::steady_clock::now() < end);
        m_mgr->exit_thread(busy_tid, 4);
    });
    worker.join();
    ASSERT_TRUE(m_mgr->get_thread_times(busy, after, error));
    ASSERT_TRUE(m_mgr->get_thread_times(idle, idle_after, error));
    EXPECT_GT(after[3], 0u); EXPECT_GE(after[2], before[2]); EXPECT_GE(after[3], before[3]);
    EXPECT_EQ(after[0], before[0]); EXPECT_GE(after[1], after[0]);
    EXPECT_EQ(idle_after[2], 0u); EXPECT_EQ(idle_after[3], 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(m_mgr->get_thread_times(busy, retained, error)); EXPECT_EQ(after, retained);
}
