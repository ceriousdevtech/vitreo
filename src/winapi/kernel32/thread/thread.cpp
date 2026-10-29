/// @file thread.cpp
/// @brief Thread creation and lifecycle implementation (THREAD-001)

#include "vitreo/guest_sentinels.hpp"
#include "winapi/kernel32/thread/thread_context.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/host_thread_name.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/common/thread_description_contract.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "core/sync/ntsync.hpp"

#include <vitreo/ab_cycle.hpp>
#include <vitreo/diagnostics.hpp>
#include <vitreo/present_cpu.hpp>

#include <cstdio>
#include <pthread.h>
#include <sys/qos.h>
#include <unistd.h>
#include <mach/mach.h>

namespace vitreo::winapi {

// Host SDK mach/thread_info.h: THREAD_BASIC_INFO reports distinct user/system
// time_value_t counters (seconds + microseconds). This owns the send right
// returned by mach_thread_self; no guest HANDLE or pointer is a Mach object.
class ThreadCpuAccounting {
public:
    ThreadCpuAccounting() : m_port(mach_thread_self()) {
        m_valid = read(m_initial);
    }
    ~ThreadCpuAccounting() { release(); }
    ThreadCpuAccounting(const ThreadCpuAccounting&) = delete;
    ThreadCpuAccounting& operator=(const ThreadCpuAccounting&) = delete;

    bool snapshot(u64& kernel, u64& user) {
        if (!m_valid) return false;
        if (m_port != MACH_PORT_NULL) {
            thread_basic_info_data_t info{};
            if (!read(info)) return false;
            m_kernel = elapsed(info.system_time, m_initial.system_time);
            m_user = elapsed(info.user_time, m_initial.user_time);
        }
        kernel = m_kernel;
        user = m_user;
        return true;
    }
    void finish() {
        u64 kernel = 0, user = 0;
        m_valid = snapshot(kernel, user);
        release();
    }
    /// The executing host thread's port while it runs (NULL once finished).
    [[nodiscard]] mach_port_t port() const noexcept { return m_port; }
private:
    bool read(thread_basic_info_data_t& info) const {
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        return m_port != MACH_PORT_NULL &&
            thread_info(m_port, THREAD_BASIC_INFO,
                        reinterpret_cast<thread_info_t>(&info), &count) == KERN_SUCCESS;
    }
    static u64 elapsed(time_value_t now, time_value_t initial) {
        // Learn GetThreadTimes: FILETIME durations use 10,000,000 ticks/s.
        const i64 us = (static_cast<i64>(now.seconds) - initial.seconds) * TIME_MICROS_MAX +
                       now.microseconds - initial.microseconds;
        return us > 0 ? static_cast<u64>(us) * 10 : 0;
    }
    void release() {
        if (m_port != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), m_port);
            m_port = MACH_PORT_NULL;
        }
    }
    mach_port_t m_port{MACH_PORT_NULL};
    thread_basic_info_data_t m_initial{};
    u64 m_kernel{0}, m_user{0};
    bool m_valid{false};
};

namespace {

u64 current_filetime() {
    constexpr u64 kWindowsEpochOffset100ns = 116444736000000000ULL;
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return kWindowsEpochOffset100ns + static_cast<u64>(nanoseconds / 100);
}

bool thread_diag_enabled() {
    return environment_flag_enabled("VITREO_THREAD_DIAG");
}

ThreadManager* g_thread_manager = nullptr;

void snapshot_threads_for_perf() {
    if (g_thread_manager != nullptr) {
        g_thread_manager->snapshot_perf_window();
    }
}

} // namespace

ThreadStackSizes compute_native_thread_stack_sizes(u64 commit, u64 reserve,
                                                   u64 default_reserve,
                                                   u64 default_commit) {
    constexpr u64 kPage = 0x1000;
    constexpr u64 kGranularity = 0x10000;
    constexpr u64 kMegabyte = 0x100000;
    auto round_up = [](u64 v, u64 a) { return (v + a - 1) & ~(a - 1); };
    ThreadStackSizes s{reserve != 0 ? reserve : default_reserve,
                       commit != 0 ? commit : default_commit};
    s.commit = round_up(s.commit == 0 ? kPage : s.commit, kPage);
    if (s.commit >= s.reserve) s.reserve = round_up(s.commit, kMegabyte);
    s.reserve = round_up(s.reserve, kGranularity);
    return s;
}

ThreadStackSizes compute_thread_stack_sizes(u64 requested, u32 creation_flags,
                                            u64 default_reserve,
                                            u64 default_commit) {
    return (creation_flags & STACK_SIZE_PARAM_IS_A_RESERVATION) != 0
               ? compute_native_thread_stack_sizes(0, requested,
                                                   default_reserve,
                                                   default_commit)
               : compute_native_thread_stack_sizes(requested, 0,
                                                   default_reserve,
                                                   default_commit);
}

// =============================================================================
// ThreadManager
// =============================================================================

ThreadManager::ThreadManager(HandleTable& handles, u32 primary_thread_id)
    : m_handles(handles) {
    if (primary_thread_id == 0) {
        if (const char* value = std::getenv("VITREO_PRIMARY_THREAD_ID")) {
            u32 parsed = 0;
            const auto result = std::from_chars(value, value + std::strlen(value), parsed);
            if (result.ec == std::errc{} && *result.ptr == '\0' && parsed != 0)
                primary_thread_id = parsed;
        }
    }
    if (primary_thread_id == 0) primary_thread_id = allocate_thread_id();
    if (primary_thread_id == 0) {
        m_main_thread_id = 0;
        set_current_thread_id(0);
        std::fprintf(stderr, "[vitreo] primary thread identity allocation failed\n");
        return;
    }
    m_next_tid.store(primary_thread_id + 1u, std::memory_order_relaxed);
    set_current_thread_id(primary_thread_id);
    g_thread_manager = this;
    diagnostics::set_perf_thread_snapshot(&snapshot_threads_for_perf);
    // Create main thread entry
    auto main_thread = std::make_unique<GuestThread>();
    main_thread->thread_id = primary_thread_id;
    main_thread->creation_time = current_filetime();
    // Manual-reset event in ntsync — fires when thread terminates.
    main_thread->nt_id = ntsync::global().create_event(/*manual_reset=*/true,
                                                      /*initial_state=*/false);
    // The primary guest thread is already executing when ThreadManager is
    // constructed. SuspendThread must therefore use the live cooperative
    // suspend path for duplicated primary-thread handles.
    main_thread->launched = true;
    m_main_thread_id = main_thread->thread_id;
    m_threads[main_thread->thread_id] = std::move(main_thread);
}

u32 ThreadManager::allocate_thread_id() {
    const u32 system_tid = ipc::allocate_system_thread_id();
    if (system_tid != 0) return system_tid;
    // A configured capsule must not fall back to a process-local namespace:
    // a failed shared allocation would silently duplicate another guest TID.
    if (const char* scope = std::getenv("VITREO_IPC_SCOPE"); scope && *scope)
        return 0;
    u32 next = m_next_tid.load(std::memory_order_relaxed);
    while (next != 0) {
        if (m_next_tid.compare_exchange_weak(next, next + 1u,
                                            std::memory_order_relaxed))
            return next;
    }
    return 0;
}

ThreadManager::~ThreadManager() {
    // Only the destroying thread's scheduling cache can be reached from here;
    // worker host threads are joined before RuntimeContext tears this down.
    if (t_scheduling_mgr == this) {
        t_scheduling_mgr = nullptr;
        t_thread_epoch = nullptr;
        t_thread_epoch_owner.reset();
    }
    diagnostics::set_perf_thread_snapshot(nullptr);
    g_thread_manager = nullptr;
    // Teardown normally joins every tracked worker first
    // (join_host_threads). A joinable std::thread left here would call
    // std::terminate; finished ones are joined, anything else is let go.
    std::lock_guard lock(m_host_threads_mutex);
    for (auto& t : m_host_threads) {
        if (!t.joinable()) continue;
        const bool done = std::find(m_finished_host_threads.begin(),
                                    m_finished_host_threads.end(),
                                    t.get_id()) != m_finished_host_threads.end();
        if (done) t.join(); else t.detach();
    }
}

void ThreadManager::snapshot_perf_window() {
    u32 active = 0;
    u32 runnable = 0;
    u32 suspended = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [tid, thread] : m_threads) {
            (void)tid;
            if (thread == nullptr || thread->terminated) continue;
            ++active;
            if (thread->suspend_count > 0) {
                ++suspended;
            } else if (thread->launched) {
                ++runnable;
            }
        }
    }
    u32 host = 0;
    {
        std::lock_guard<std::mutex> lock(m_host_threads_mutex);
        host = static_cast<u32>(m_host_threads.size());
    }
    diagnostics::perf_note_threads(active, runnable, suspended, host);
}

void ThreadManager::track_host_thread(std::thread thread) {
    std::vector<std::thread> finished;
    {
        std::lock_guard lock(m_host_threads_mutex);
        if (!m_finished_host_threads.empty()) {
            for (auto it = m_host_threads.begin(); it != m_host_threads.end();) {
                const auto done = std::find(m_finished_host_threads.begin(),
                                            m_finished_host_threads.end(),
                                            it->get_id());
                if (done != m_finished_host_threads.end()) {
                    m_finished_host_threads.erase(done);
                    finished.push_back(std::move(*it));
                    it = m_host_threads.erase(it);
                } else {
                    ++it;
                }
            }
        }
        m_host_threads.push_back(std::move(thread));
    }
    // Each of these has returned from its guest entry point and is at most a
    // few instructions from exiting, so the joins do not block.
    for (auto& t : finished) {
        if (t.joinable() && t.get_id() != std::this_thread::get_id()) t.join();
        else if (t.joinable()) t.detach();
    }
}

void ThreadManager::begin_shutdown() {
    m_shutdown.store(true, std::memory_order_release);
    // Windows process termination ends every guest thread regardless of its
    // suspend count. A thread the guest suspended and never resumed would
    // otherwise stay parked forever and block join_host_threads().
    ThreadSuspendRegistry::instance().release_all_for_shutdown();
}

void ThreadManager::join_host_threads() {
    for (;;) {
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(m_host_threads_mutex);
            if (m_host_threads.empty()) break;
            threads.swap(m_host_threads);
        }
        for (auto& thread : threads) {
            if (thread.joinable()) thread.join();
        }
    }
}

Handle ThreadManager::create_thread(usize stack_size, GuestAddr start_address,
                                     GuestAddr parameter, u32 creation_flags,
                                     u32* thread_id_out,
                                     u64 second_parameter,
                                     u32 handle_flags,
                                     std::optional<ThreadStackSizes>
                                         native_stack) {
    // A NULL (or otherwise unusable) start address is not validated: Learn
    // CreateThread "may succeed even if lpStartAddress points to data, code,
    // or is not accessible" and the thread then faults
    // (thread_lifecycle_probe CREATE_NULL_START ok=1).
    auto thread = std::make_unique<GuestThread>();
    thread->ideal_processor =
        m_next_ideal_processor.fetch_add(1, std::memory_order_relaxed) %
        std::max<u32>(1, static_cast<u32>(platform::host_hardware().cpu.processors.size()));
    // Windows TIDs are system-wide. With a capsule IPC scope, allocate from the
    // shared counter so peer GetWindowThreadProcessId / PostThreadMessage never
    // collide with another process's local sequence. Without a scope (unit
    // tests), keep the process-local counter starting after the primary's 1.
    thread->thread_id = allocate_thread_id();
    if (thread->thread_id == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return NULL_HANDLE;
    }
    thread->start_address = start_address;
    thread->parameter     = parameter;
    const ThreadStackSizes stack =
        native_stack
            ? compute_native_thread_stack_sizes(
                  native_stack->commit, native_stack->reserve,
                  m_default_stack_reserve, m_default_stack_commit)
            : compute_thread_stack_sizes(stack_size, creation_flags,
                                         m_default_stack_reserve,
                                         m_default_stack_commit);
    thread->stack_size    = static_cast<usize>(stack.reserve);
    thread->stack_commit  = static_cast<usize>(stack.commit);
    thread->creation_time = current_filetime();
    thread->suspend_count = (creation_flags & CREATE_SUSPENDED) ? 1u : 0u;

    // Without a runtime (unit tests) the start context has no stack yet.
    thread->cpu.init_long_mode();
    seed_windows_initial_fpu(thread->cpu);
    thread->cpu.rip = start_address;
    thread->cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RCX), parameter);
    thread->cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RDX), second_parameter);

    // Windows builds the stack and TEB inside CreateThread; running out of
    // memory there fails the call (Learn CreateThread / "Thread Stack Size").
    if (m_prepare_callback && !m_prepare_callback(*thread)) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return NULL_HANDLE;
    }
    thread->nt_id = ntsync::global().create_event(/*manual_reset=*/true,
                                                  /*initial_state=*/false);

    const u32 tid = thread->thread_id;
    GuestThread* raw = thread.get();
    {
        std::lock_guard lock(m_mutex);
        m_threads[tid] = std::move(thread);
    }
    if (thread_id_out) {
        *thread_id_out = tid;
    }

    // Create handle pointing to the GuestThread object. handle_flags carries
    // HANDLE_FLAG_INHERIT from SECURITY_ATTRIBUTES (thread_lifecycle_probe
    // CREATE_INHERIT).
    Handle h = m_handles.allocate(HandleType::Thread, raw, handle_flags);
    if (thread_diag_enabled()) {
        std::fprintf(
            stderr,
            "[thread-diag] pid=%d create self=%u target=%u handle=0x%x "
            "start=0x%llx param=0x%llx flags=0x%x requested_stack=0x%zx "
            "effective_stack=0x%zx launched=0 nt=0x%llx\n",
            static_cast<int>(::getpid()), get_current_thread_id(), tid, h,
            static_cast<unsigned long long>(start_address),
            static_cast<unsigned long long>(parameter), creation_flags,
            stack_size, raw->stack_size,
            static_cast<unsigned long long>(raw->nt_id));
    }
    return h;
}

void ThreadManager::seed_start_context(cpu::CpuState& cpu, GuestAddr start,
                                       GuestAddr parameter) const {
    // Measured (thread_lifecycle_probe SUSPENDED_START): x64 RIP =
    // RtlUserThreadStart, RCX = start, RDX = parameter, every other GPR 0,
    // EFLAGS 0x200, RSP = 8 mod 16 inside the new stack; x86 EIP =
    // RtlUserThreadStart, EAX = start, EBX = parameter, EFLAGS 0x202,
    // ESP 4-aligned.
    for (u8 reg = 0; reg < 16; ++reg) {
        if (reg != static_cast<u8>(cpu::RegIndex::RSP)) cpu.gpr.set64(reg, 0);
    }
    cpu.discard_lazy_flags();
    if (cpu.is_64bit()) {
        cpu.flags.rflags = 0x200;
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RCX), start);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RDX), parameter);
        cpu.set_sp(((cpu.sp() & ~u64{0xF}) - 0x30) | 0x8);
    } else {
        cpu.flags.rflags = 0x202;
        cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::RAX), static_cast<u32>(start));
        cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::RBX), static_cast<u32>(parameter));
    }
    cpu.rip = m_user_thread_start != 0 ? m_user_thread_start : start;
}

void ThreadManager::user_thread_start(Marshaler& m) const {
    auto* cpu = m.cpu();
    if (cpu == nullptr) return;
    // ThreadProc's return value is in RAX/EAX. Windows RtlUserThreadStart
    // moves that into the RtlExitUserThread Status argument; returning
    // straight into RtlExitUserThread would read RCX/the stack slot (still
    // the thread parameter — thread_lifecycle_probe START_REG_REDIRECT
    // exit=5678). Return into the callback sentinel so the runner takes
    // RAX as the exit code, matching a normal ThreadProc return.
    auto& memory = m.memory();
    const GuestAddr ret = callback_return_sentinel(cpu->is_64bit());
    if (cpu->is_64bit()) {
        const GuestAddr start = cpu->gpr.get64(static_cast<u8>(cpu::RegIndex::RCX));
        const GuestAddr param = cpu->gpr.get64(static_cast<u8>(cpu::RegIndex::RDX));
        // CALL frame: shadow space, return address at RSP = 8 mod 16.
        const u64 sp = (cpu->sp() & ~u64{0xF}) - 0x28;
        if (memory.write_u64(sp, ret).is_err()) return;
        cpu->set_sp(sp);
        cpu->gpr.set64(static_cast<u8>(cpu::RegIndex::RCX), param);
        cpu->rip = start;
    } else {
        const u32 start = cpu->gpr.get32(static_cast<u8>(cpu::RegIndex::RAX));
        const u32 param = cpu->gpr.get32(static_cast<u8>(cpu::RegIndex::RBX));
        const u64 sp = cpu->sp() - 8;
        if (memory.write_u32(sp + 4, param).is_err() ||
            memory.write_u32(sp, static_cast<u32>(ret)).is_err()) {
            return;
        }
        cpu->set_sp(sp);
        cpu->rip = start;
    }
    cpu->thunk_control_transfer = true;
}

void ThreadManager::exit_thread(u32 thread_id, u32 exit_code) {
    // TerminateThread already recorded the exit code, signaled the object and
    // ran the exit callback; the host thread is only unwinding now.
    if (!m_shutdown.load(std::memory_order_acquire)) {
        std::lock_guard lock(m_mutex);
        auto it = m_threads.find(thread_id);
        if (it != m_threads.end() && it->second && it->second->terminated) {
            return;
        }
    }
    // Notified before the shutdown check: a waiter blocked on this thread's
    // completion must be released even while the process is tearing down.
    if (m_thread_exit_callback) m_thread_exit_callback(thread_id, exit_code);
    m_last_exit_code.store(exit_code, std::memory_order_release);
    if (m_shutdown.load(std::memory_order_acquire)) {
        return;  // Process is shutting down; objects may be destroyed.
    }
    std::uint64_t nt_id = 0;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_threads.find(thread_id);
        if (it == m_threads.end()) {
            return;
        }
        GuestThread* gt = it->second.get();
        if (gt->cpu_accounting) gt->cpu_accounting->finish();
        gt->exit_code  = exit_code;
        gt->exit_time  = current_filetime();
        gt->terminated = true;
        nt_id = gt->nt_id;
    }
    // Signal the thread's completion event — wakes all waiters atomically
    // via the ntsync manager (no per-thread mutex / cv needed).
    if (nt_id != 0) ntsync::global().set_event(nt_id);
    if (thread_diag_enabled()) {
        std::fprintf(
            stderr,
            "[thread-diag] pid=%d exit tid=%u code=%u nt=0x%llx signaled=%d\n",
            static_cast<int>(::getpid()), thread_id, exit_code,
            static_cast<unsigned long long>(nt_id),
            nt_id != 0 ? 1 : 0);
    }
    // Abandon any Win32 mutexes still owned by this thread, per Windows
    // semantics: next waiter receives WAIT_ABANDONED.
    ntsync::global().abandon_for_thread(thread_id);

    // A thread object lives while it runs or while a handle refers to it
    // (Learn, CloseHandle / "Terminating a Thread"). A thread whose handles
    // were all closed while it ran — every pool callback thread, and the
    // common CloseHandle(CreateThread(...)) — is destroyed now; before, its
    // GuestThread and completion event stayed until process exit.
    std::uint64_t orphan_nt = 0;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_threads.find(thread_id);
        if (it != m_threads.end() && it->second &&
            m_handles.count_object(it->second.get(), HandleType::Thread) == 0) {
            orphan_nt = it->second->nt_id;
            m_thread_descriptions.erase(thread_id);
            m_threads.erase(it);
        }
    }
    if (orphan_nt != 0) ntsync::global().destroy(orphan_nt);
}

void ThreadManager::bind_current_host_thread(u32 thread_id) {
    std::lock_guard lock(m_mutex);
    const auto it = m_threads.find(thread_id);
    if (it != m_threads.end() && it->second && !it->second->terminated) {
        it->second->cpu_accounting = std::make_shared<ThreadCpuAccounting>();
        // [present-cpu] (VITREO_PRESENT_RATE): a thread named before it ran
        // gets its port now. No-op for an unnamed thread or with the meter off.
        diagnostics::present_cpu::note_thread_port(thread_id);
    }
}

void ThreadManager::finish_thread_cpu_accounting(u32 thread_id) {
    std::lock_guard lock(m_mutex);
    const auto it = m_threads.find(thread_id);
    if (it != m_threads.end() && it->second && it->second->cpu_accounting)
        it->second->cpu_accounting->finish();
}

bool ThreadManager::get_thread_times(Handle handle, std::array<u64, 4>& times,
                                     u32& error) {
    std::lock_guard lock(m_mutex);
    GuestThread* thread = resolve_thread_handle(handle, THREAD_QUERY_LIMITED_INFORMATION, error);
    if (!thread) return false;
    std::array<u64, 4> snapshot{thread->creation_time, thread->exit_time, 0, 0};
    if (thread->cpu_accounting &&
        !thread->cpu_accounting->snapshot(snapshot[2], snapshot[3])) {
        error = error_codes::ERROR_CALL_NOT_IMPLEMENTED;
        return false;
    }
    times = snapshot;
    return true;
}

bool ThreadManager::is_thread_terminated(u32 thread_id) const {
    std::lock_guard lock(m_mutex);
    auto it = m_threads.find(thread_id);
    return it != m_threads.end() && it->second && it->second->terminated;
}

bool ThreadManager::any_live_thread_except(u32 self) const {
    std::lock_guard lock(m_mutex);
    for (const auto& [tid, thread] : m_threads) {
        if (tid != self && thread && !thread->terminated) return true;
    }
    return false;
}

void ThreadManager::add_service_thread(u32 thread_id) {
    bool first = false;
    {
        std::lock_guard lock(m_mutex);
        first = ++m_service_threads[thread_id] == 1;
    }
    // A fresh thread under a reused id starts with a clean slate (an alert
    // sent to the previous holder after it unregistered is not inherited).
    if (first && m_service_thread_id_callback) m_service_thread_id_callback(thread_id);
}

void ThreadManager::remove_service_thread(u32 thread_id) {
    bool last = false;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_service_threads.find(thread_id);
        if (it == m_service_threads.end()) return;
        if (--it->second == 0) {
            m_service_threads.erase(it);
            last = true;
        }
    }
    if (last && m_service_thread_id_callback) m_service_thread_id_callback(thread_id);
}

bool ThreadManager::is_thread_of_this_process(u32 thread_id) const {
    std::lock_guard lock(m_mutex);
    return m_threads.find(thread_id) != m_threads.end() ||
           m_service_threads.find(thread_id) != m_service_threads.end();
}

void ThreadManager::exit_current_thread_noreturn(Marshaler& m, u32 exit_code) {
    // ExitThread / FreeLibraryAndExitThread never return. Redirect guest RIP
    // to the callback or process return sentinel so MSVC noreturn `int3`
    // padding after the call is never executed.
    //
    // The thread object is NOT signaled here. Learn ExitThread: DLL_THREAD_
    // DETACH notifications run as part of the exit and only then does the
    // thread object become signaled (thread_lifecycle_probe:
    // detach_before_signal=1). Whoever runs this thread signals it after the
    // detach notifications, with the exit code left in RAX.
    const u32 tid = get_current_thread_id();

    auto* cpu = m.cpu();
    if (cpu == nullptr) {
        return;
    }

    if (cpu->is_64bit()) {
        cpu->gpr.set64(static_cast<u8>(cpu::RegIndex::RAX), exit_code);
    } else {
        cpu->gpr.set32(static_cast<u8>(cpu::RegIndex::RAX), exit_code);
    }

    cpu->thunk_control_transfer = true;
    // The primary thread leaves its run loop through the process sentinel.
    // Unless it is the last thread, that is NOT process exit: the runtime
    // sees primary_exit_pending(), runs its detach notifications, signals it
    // and keeps the process alive until the last thread exits (Learn
    // ExitThread). Any other thread unwinds through the callback loop.
    if (tid == main_thread_id() && any_live_thread_except(tid)) {
        m_primary_exit_code = exit_code;
        m_primary_exit_pending.store(true, std::memory_order_release);
    }
    cpu->rip = (tid == main_thread_id())
        ? process_return_sentinel(cpu->is_64bit())
        : callback_return_sentinel(cpu->is_64bit());

    VITREO_TRACE(
        "[thread] ExitThread(tid=%u code=%u) -> noreturn rip=0x%llx\n", tid,
        exit_code, static_cast<unsigned long long>(cpu->rip));
    ipc::ipc_log("ExitThread(tid=%u code=0x%x) noreturn rip=0x%llx", tid,
                 exit_code, static_cast<unsigned long long>(cpu->rip));
}

Result<u32, Error> ThreadManager::get_exit_code(Handle thread_handle) const {
    std::lock_guard lock(m_mutex);

    auto* gt = m_handles.get_as<GuestThread>(thread_handle, HandleType::Thread);
    if (!gt) {
        return Result<u32, Error>::err(Error::InvalidHandle);
    }
    return Result<u32, Error>::ok(gt->exit_code);
}

bool ThreadManager::terminate_thread(Handle thread_handle, u32 exit_code) {
    std::uint64_t nt_id = 0;
    u32 tid = 0;
    bool running = false;
    {
        std::lock_guard lock(m_mutex);

        u32 error = 0;
        auto* gt = resolve_thread_handle(thread_handle, THREAD_TERMINATE, error);
        if (!gt) {
            set_last_error(error);
            return false;
        }
        if (gt->terminated) return true;

        if (gt->cpu_accounting) gt->cpu_accounting->finish();
        gt->exit_code  = exit_code;
        gt->exit_time  = current_filetime();
        gt->terminated = true;
        nt_id = gt->nt_id;
        tid   = gt->thread_id;
        running = gt->launched;
        // A thread that never ran never will: free what CreateThread built.
        if (!gt->launched) {
            gt->launched = true;  // ResumeThread must not start it now
            if (m_release_callback) m_release_callback(*gt);
            gt->launch_context.reset();
        }
        // Learn ExitThread/TerminateThread: only the last thread's end ends
        // the process, so a terminated primary thread leaves it running.
        if (running && tid == m_main_thread_id) {
            bool others = false;
            for (const auto& [other, thread] : m_threads) {
                if (other != tid && thread && !thread->terminated) {
                    others = true;
                    break;
                }
            }
            if (others) {
                m_primary_exit_code = exit_code;
                m_primary_exit_pending.store(true, std::memory_order_release);
            }
        }
    }
    if (running) {
        ThreadSuspendRegistry::instance().request_terminate(tid);
        ntsync::global().interrupt_thread(tid);
    }
    m_last_exit_code.store(exit_code, std::memory_order_release);
    if (nt_id != 0) ntsync::global().set_event(nt_id);
    if (m_thread_exit_callback) m_thread_exit_callback(tid, exit_code);
    ntsync::global().abandon_for_thread(tid);
    return true;
}

GuestThread* ThreadManager::resolve_thread_handle(Handle h, u32 required_access,
                                                  u32& error) {
    // Caller holds m_mutex.
    if (h == get_current_thread()) {
        auto it = m_threads.find(t_current_guest_tid);
        if (it != m_threads.end() && it->second) return it->second.get();
        error = error_codes::ERROR_INVALID_HANDLE;
        return nullptr;
    }
    GuestThread* gt = m_handles.get_as<GuestThread>(h, HandleType::Thread);
    if (gt == nullptr) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return nullptr;
    }
    if (required_access != 0) {
        u32 granted = m_handles.get_access(h);
        // Windows grants the LIMITED rights with the full ones.
        if (granted & THREAD_QUERY_INFORMATION) granted |= THREAD_QUERY_LIMITED_INFORMATION;
        if (granted & THREAD_SET_INFORMATION) granted |= THREAD_SET_LIMITED_INFORMATION;
        if ((granted & required_access) != required_access) {
            error = error_codes::ERROR_ACCESS_DENIED;
            return nullptr;
        }
    }
    return gt;
}

// ThreadHideFromDebugger: oracle thread_hide_probe.c; full rights required.
u32 ThreadManager::set_thread_hidden_from_debugger(Handle h) {
    std::lock_guard lock(m_mutex);
    u32 error = 0;
    auto* thread = resolve_thread_handle(h, THREAD_SET_INFORMATION, error);
    if (thread == nullptr) return error;
    thread->hidden_from_debugger = true;
    return 0;
}

bool ThreadManager::current_thread_skips_thread_attach() {
    if (g_thread_manager == nullptr) return false;
    const u32 tid = get_current_thread_id_static();
    std::lock_guard lock(g_thread_manager->m_mutex);
    const auto it = g_thread_manager->m_threads.find(tid);
    return it != g_thread_manager->m_threads.end() && it->second->skip_thread_attach;
}

u32 ThreadManager::get_thread_hidden_from_debugger(Handle h, bool& hidden) {
    std::lock_guard lock(m_mutex);
    u32 error = 0;
    auto* thread = resolve_thread_handle(h, THREAD_QUERY_INFORMATION, error);
    if (thread == nullptr) return error;
    hidden = thread->hidden_from_debugger;
    return 0;
}

// Learn SetThreadPriority lists the accepted levels; everything else fails.
// Measured (thread_lifecycle_probe SETPRIO / BG_*): -15, -2..2 and 15 are
// accepted and read back; any other value -> ERROR_INVALID_PARAMETER with the
// old level kept; THREAD_MODE_BACKGROUND_BEGIN/END only on the calling thread
// (another thread -> ERROR_INVALID_PARAMETER), twice -> ERROR_THREAD_MODE_
// ALREADY_BACKGROUND (400) / ERROR_THREAD_MODE_NOT_BACKGROUND (401), and
// GetThreadPriority reads -4 while in background mode. Requires
// THREAD_SET_(LIMITED_)INFORMATION; GetThreadPriority THREAD_QUERY_LIMITED_
// INFORMATION.
// Learn, "Scheduling Priorities": a thread's base priority is fixed by its
// process's priority class and its own level. Levels: IDLE -15, LOWEST -2,
// BELOW_NORMAL -1, NORMAL 0, ABOVE_NORMAL 1, HIGHEST 2, TIME_CRITICAL 15.
int windows_base_priority(u32 priority_class, i32 level) noexcept {
    constexpr u32 kIdle = 0x40u, kBelowNormal = 0x4000u,
                  kAboveNormal = 0x8000u, kHigh = 0x80u, kRealtime = 0x100u;
    if (priority_class == kRealtime) {
        if (level == -15) return 16;
        if (level == 15) return 31;
        return 24 + level;
    }
    if (level == -15) return 1;
    if (level == 15) return 15;
    const int base = priority_class == kIdle          ? 4
                   : priority_class == kBelowNormal   ? 6
                   : priority_class == kAboveNormal   ? 10
                   : priority_class == kHigh          ? 13
                                                      : 8;   // NORMAL
    return base + level;
}

// The host has no priority numbers to set, only QoS classes, which also
// decide P- versus E-core placement on Apple silicon. NORMAL (8) is the level
// of an ordinary foreground thread, so it is the class the system gives work a
// user is waiting on; anything above it is the interactive band; BELOW_NORMAL
// keeps the class every guest thread had before priorities were applied.
// Explicit EcoQoS ("always ... efficient cores", Learn "Quality of Service")
// is UTILITY, not BACKGROUND, which would also throttle the thread's I/O.
// sys/qos.h orders the classes by value, so max() raises one to a floor.
unsigned host_qos_class_for(int base, bool background, PowerThrottlingQos qos,
                            HostQosFloor floor) noexcept {
    if (background || base <= 1) return QOS_CLASS_BACKGROUND;
    if (qos == PowerThrottlingQos::Eco) return QOS_CLASS_UTILITY;
    qos_class_t cls = base >= 9   ? QOS_CLASS_USER_INTERACTIVE
                      : base == 8 ? QOS_CLASS_USER_INITIATED
                      : base == 7 ? QOS_CLASS_DEFAULT
                                  : QOS_CLASS_UTILITY;
    if (base <= 7) {
        const qos_class_t lowest = floor == HostQosFloor::UserInitiated
                                       ? QOS_CLASS_USER_INITIATED
                                   : floor == HostQosFloor::Default
                                       ? QOS_CLASS_DEFAULT
                                       : QOS_CLASS_UTILITY;
        cls = std::max(cls, lowest);
    }
    if (qos == PowerThrottlingQos::High) {
        cls = std::max(cls, QOS_CLASS_USER_INITIATED);
    }
    return cls;
}

u32 validate_thread_power_throttling(u32 size, u32 version, u32 control,
                                     u32 state) noexcept {
    if (size < 12 || version > POWER_THROTTLING_CURRENT_VERSION ||
        (control & ~POWER_THROTTLING_EXECUTION_SPEED) != 0 ||
        (state & ~control) != 0) {
        return error_codes::ERROR_INVALID_PARAMETER;
    }
    return 0;
}

u32 validate_process_power_throttling(u32 size, u32 version, u32 control,
                                      u32 state) noexcept {
    constexpr u32 kAcceptedControl = 0x7u;
    if (size != 12) return error_codes::ERROR_BAD_LENGTH;
    if (version != POWER_THROTTLING_CURRENT_VERSION ||
        (control & ~kAcceptedControl) != 0 || (state & ~control) != 0) {
        return error_codes::ERROR_INVALID_PARAMETER;
    }
    return 0;
}

namespace {

[[nodiscard]] const char* qos_floor_name(HostQosFloor f) noexcept {
    return f == HostQosFloor::UserInitiated ? "user_initiated"
           : f == HostQosFloor::Default     ? "default"
                                            : "utility";
}

// Under VITREO_AB_CYCLE_S with the "qos-floor" arm in play, one host thread
// sleeps to each window boundary and bumps the host policy epoch, so every
// guest thread re-applies its class at its next wait in the new window.
void start_qos_floor_arm_watcher() {
    static std::once_flag once;
    std::call_once(once, [] {
        const u64 period = diagnostics::ab_detail::period_seconds();
        if (period == 0 ||
            ThreadManager::configured_qos_floor() == HostQosFloor::Utility) {
            return;
        }
        const char* only = std::getenv("VITREO_AB_CYCLE_ONLY");
        if (only != nullptr && only[0] != '\0' &&
            !diagnostics::ab_detail::listed(only, "qos-floor")) {
            return;
        }
        std::thread([period] {
            const auto start = diagnostics::ab_detail::origin();
            for (u64 window = 1;; ++window) {
                std::this_thread::sleep_until(
                    start + std::chrono::seconds(period * window) +
                    std::chrono::milliseconds(5));
                ThreadManager::notify_host_scheduling_policy_changed();
                std::fprintf(stderr, "[thread-qos] arm switch: floor=%s, "
                             "each guest thread re-applies at its next wait\n",
                             qos_floor_name(ThreadManager::active_qos_floor()));
            }
        }).detach();
    });
}

}  // namespace

HostQosFloor ThreadManager::configured_qos_floor() noexcept {
    const int forced = s_qos_floor_override.load(std::memory_order_relaxed);
    if (forced >= 0) return static_cast<HostQosFloor>(forced);
    static const HostQosFloor floor = [] {
        const char* v = std::getenv("VITREO_THREAD_QOS_FLOOR");
        if (v == nullptr || v[0] == '\0') return HostQosFloor::UserInitiated;
        HostQosFloor f = HostQosFloor::Utility;
        if (std::strcmp(v, "user_initiated") == 0) {
            f = HostQosFloor::UserInitiated;
        } else if (std::strcmp(v, "default") == 0) {
            f = HostQosFloor::Default;
        } else if (std::strcmp(v, "utility") != 0) {
            std::fprintf(stderr, "[thread-qos] VITREO_THREAD_QOS_FLOOR=%s not "
                         "one of user_initiated|default|utility; using "
                         "utility\n", v);
        }
        std::fprintf(stderr, "[thread-qos] floor=%s for guest base priority "
                     "2..7\n", qos_floor_name(f));
        return f;
    }();
    return floor;
}

HostQosFloor ThreadManager::active_qos_floor() noexcept {
    const HostQosFloor floor = configured_qos_floor();
    if (floor == HostQosFloor::Utility) return floor;
    return diagnostics::ab_cycle_off("qos-floor") ? HostQosFloor::Utility
                                                  : floor;
}

void ThreadManager::apply_current_thread_scheduling() {
    // Epochs first: a change landing after the reads below bumps one again,
    // so this thread re-applies at its next refresh instead of missing it.
    const u32 epoch = m_scheduling_epoch.load(std::memory_order_acquire);
    const u32 policy_epoch = s_host_policy_epoch.load(std::memory_order_acquire);
    i32 level = 0;
    bool thread_background = false;
    PowerThrottlingQos thread_qos = PowerThrottlingQos::SystemManaged;
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_threads.find(t_current_guest_tid);
        if (it == m_threads.end() || !it->second) return;
        if (t_thread_epoch_owner != it->second->scheduling_epoch) {
            t_thread_epoch_owner = it->second->scheduling_epoch;
            t_thread_epoch = t_thread_epoch_owner.get();
            // First time this host thread runs this guest thread: its
            // affinity decides which processor it reports running on.
            platform::bind_current_thread_affinity(it->second->affinity);
        }
        t_thread_epoch_seen = t_thread_epoch->load(std::memory_order_acquire);
        level = it->second->priority;
        thread_background = it->second->background;
        thread_qos = it->second->power_throttling;
    }
    t_scheduling_mgr = this;
    t_scheduling_epoch = epoch;
    t_host_policy_epoch = policy_epoch;
    start_qos_floor_arm_watcher();
    // A thread's own EXECUTION_SPEED control overrides its process's.
    const PowerThrottlingQos qos =
        thread_qos != PowerThrottlingQos::SystemManaged
            ? thread_qos
            : static_cast<PowerThrottlingQos>(
                  m_process_power_throttling.load(std::memory_order_acquire));
    const unsigned cls = host_qos_class_for(
        windows_base_priority(
            m_process_priority_class.load(std::memory_order_acquire), level),
        thread_background ||
            m_process_background.load(std::memory_order_acquire),
        qos, active_qos_floor());
    (void)pthread_set_qos_class_self_np(static_cast<qos_class_t>(cls), 0);
}

void ThreadManager::set_process_scheduling(u32 priority_class,
                                           bool background) {
    m_process_priority_class.store(priority_class, std::memory_order_release);
    m_process_background.store(background, std::memory_order_release);
    m_scheduling_epoch.fetch_add(1, std::memory_order_acq_rel);
    apply_current_thread_scheduling();
}

void ThreadManager::set_process_power_throttling(u32 control, u32 state) {
    m_process_power_throttling.store(
        static_cast<u8>(power_throttling_qos(control, state)),
        std::memory_order_release);
    m_scheduling_epoch.fetch_add(1, std::memory_order_acq_rel);
    apply_current_thread_scheduling();
}

u32 ThreadManager::set_thread_power_throttling(Handle h, u32 control,
                                               u32 state) {
    bool self = false;
    {
        std::lock_guard lock(m_mutex);
        u32 error = 0;
        GuestThread* gt = resolve_thread_handle(h, THREAD_SET_INFORMATION, error);
        if (gt == nullptr) return error;
        self = gt->thread_id == t_current_guest_tid;
        gt->power_throttling = power_throttling_qos(control, state);
        gt->scheduling_epoch->fetch_add(1, std::memory_order_acq_rel);
    }
    if (self) apply_current_thread_scheduling();
    return 0;
}

u32 ThreadManager::set_thread_memory_priority(Handle h, u32 priority) {
    std::lock_guard lock(m_mutex);
    u32 error = 0;
    GuestThread* gt = resolve_thread_handle(h, THREAD_SET_INFORMATION, error);
    if (gt == nullptr) return error;
    gt->memory_priority = priority;
    return 0;
}

u32 ThreadManager::set_thread_priority(Handle h, i32 priority) {
    bool self = false;
    const u32 error = set_thread_priority_locked(h, priority, self);
    if (error != 0) return error;
    // The level is the thread's scheduling, not just a stored number: the
    // calling thread takes it now, any other thread at its next wait (its
    // epoch was bumped under the lock).
    if (self) apply_current_thread_scheduling();
    return 0;
}

u32 ThreadManager::set_thread_priority_locked(Handle h, i32 priority,
                                              bool& self) {
    constexpr i32 kBackgroundBegin = 0x10000;
    constexpr i32 kBackgroundEnd = 0x20000;
    std::lock_guard lock(m_mutex);
    u32 error = 0;
    GuestThread* gt =
        resolve_thread_handle(h, THREAD_SET_LIMITED_INFORMATION, error);
    if (gt == nullptr) return error;
    self = gt->thread_id == t_current_guest_tid;
    if (priority == kBackgroundBegin || priority == kBackgroundEnd) {
        if (gt->thread_id != t_current_guest_tid) {
            return error_codes::ERROR_INVALID_PARAMETER;
        }
        const bool begin = priority == kBackgroundBegin;
        if (gt->background == begin) {
            return begin ? 400u : 401u;  // ERROR_THREAD_MODE_{ALREADY,NOT}_BACKGROUND
        }
        gt->background = begin;
        gt->scheduling_epoch->fetch_add(1, std::memory_order_acq_rel);
        return 0;
    }
    const bool valid = priority == -15 || priority == 15 ||
                       (priority >= -2 && priority <= 2);
    if (!valid) return error_codes::ERROR_INVALID_PARAMETER;
    gt->priority = priority;
    gt->scheduling_epoch->fetch_add(1, std::memory_order_acq_rel);
    static const bool prio_diag =
        std::getenv("VITREO_THREAD_PRIORITY_DIAG") != nullptr;
    if (prio_diag) {
        std::fprintf(stderr, "[thread-prio] set tid=%u caller=%u prio=%d launched=%d\n",
                     gt->thread_id, t_current_guest_tid, priority,
                     gt->launched ? 1 : 0);
    }
    return 0;
}

i32 ThreadManager::get_thread_priority(Handle h) {
    constexpr i32 kErrorReturn = 0x7FFFFFFF;  // THREAD_PRIORITY_ERROR_RETURN
    std::lock_guard lock(m_mutex);
    u32 error = 0;
    GuestThread* gt =
        resolve_thread_handle(h, THREAD_QUERY_LIMITED_INFORMATION, error);
    if (gt == nullptr) {
        set_last_error(error);
        return kErrorReturn;
    }
    return gt->background ? -4 : gt->priority;
}

u32 ThreadManager::set_thread_affinity(Handle h, u64 mask, u64& previous) {
    // Learn (SetThreadAffinityMask, SetThreadGroupAffinity): the handle needs
    // a set right and a query right; the limited rights suffice.
    std::lock_guard lock(m_mutex);
    u32 error = 0;
    GuestThread* gt = resolve_thread_handle(
        h, THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, error);
    if (gt == nullptr) return error;
    previous = gt->affinity->exchange(mask, std::memory_order_acq_rel);
    return 0;
}

u32 ThreadManager::get_thread_affinity(Handle h, u64& mask) {
    std::lock_guard lock(m_mutex);
    u32 error = 0;
    GuestThread* gt = resolve_thread_handle(h, THREAD_QUERY_LIMITED_INFORMATION, error);
    if (gt == nullptr) return error;
    mask = gt->affinity->load(std::memory_order_acquire);
    return 0;
}

u32 ThreadManager::thread_ideal_processor(Handle h, u32 required_access, bool set,
                                          u32 requested, u32& previous) {
    std::lock_guard lock(m_mutex);
    u32 error = 0;
    GuestThread* gt = resolve_thread_handle(h, required_access, error);
    if (gt == nullptr) return error;
    previous = gt->ideal_processor;
    if (set) gt->ideal_processor = requested;
    return 0;
}

u32 ThreadManager::thread_handle_error(Handle h, u32 required_access) {
    std::lock_guard lock(m_mutex);
    u32 error = 0;
    return resolve_thread_handle(h, required_access, error) != nullptr ? 0 : error;
}

u32 ThreadManager::thread_id_for_handle(Handle h, u32 required_access,
                                        u32& error) {
    std::lock_guard lock(m_mutex);
    error = 0;
    GuestThread* gt = resolve_thread_handle(h, required_access, error);
    return gt != nullptr ? gt->thread_id : 0;
}

u32 ThreadManager::resume_thread(Handle thread_handle) {
    if (m_external_resume_callback) {
        const auto external = m_external_resume_callback(thread_handle);
        if (external.has_value()) return *external;
    }
    bool needs_launch = false;
    bool needs_unpark = false;
    u32 tid = 0;
    u32 running_tid = 0;
    GuestAddr start_addr = 0;
    GuestAddr param = 0;
    u32 prev = 0;
    {
        std::lock_guard lock(m_mutex);

        // Learn ResumeThread: failure is (DWORD)-1 with GetLastError. Measured
        // (thread_lifecycle_probe): bad handle -> ERROR_INVALID_HANDLE,
        // no THREAD_SUSPEND_RESUME -> ERROR_ACCESS_DENIED; an exited thread
        // SUCCEEDS with 0; the pseudo handle names the caller.
        u32 error = 0;
        auto* gt = resolve_thread_handle(thread_handle, THREAD_SUSPEND_RESUME,
                                         error);
        if (!gt) {
            set_last_error(error);
            return ~0u;
        }
        // A terminated thread counts as exited, including one terminated
        // while CREATE_SUSPENDED (terminate_thread_probe SUSPENDED resume=0).
        if (gt->terminated) return 0;

        // CV-07: Return previous suspend count. Decrement toward 0.
        prev = gt->suspend_count;
        if (gt->suspend_count > 0) {
            gt->suspend_count--;
        }

        // If the thread was created suspended and never launched, launch it now
        if (gt->suspend_count == 0 && !gt->launched) {
            gt->launched = true;
            needs_launch = true;
            tid = gt->thread_id;
            start_addr = gt->start_address;
            param = gt->parameter;
        } else if (gt->launched) {
            // A running thread that was cooperatively parked: wake it.
            needs_unpark = true;
            running_tid = gt->thread_id;
        }
    }
    if (needs_unpark) {
        ThreadSuspendRegistry::instance().request_resume(running_tid);
    }
    if (needs_launch) {
        VITREO_TRACE("[thread] ResumeThread launching suspended tid=%u start=0x%llx\n",
                    tid, static_cast<unsigned long long>(start_addr));
        run_async(tid, start_addr, param);
    }
    return prev;
}

u32 ThreadManager::suspend_thread(Handle thread_handle,
                                  SuspendResult* status_out) {
    u32 prev;
    u32 tid = 0;
    bool launched = false;
    auto report = [status_out](SuspendResult r) {
        if (status_out) *status_out = r;
    };
    {
        std::lock_guard lock(m_mutex);

        // Learn SuspendThread: failure is (DWORD)-1 with GetLastError.
        // Measured (thread_lifecycle_probe): bad handle -> ERROR_INVALID_
        // HANDLE, no THREAD_SUSPEND_RESUME / exited -> ERROR_ACCESS_DENIED,
        // at MAXIMUM_SUSPEND_COUNT -> ERROR_SIGNAL_REFUSED; the pseudo handle
        // suspends the caller.
        u32 error = 0;
        auto* gt = resolve_thread_handle(thread_handle, THREAD_SUSPEND_RESUME,
                                         error);
        if (!gt) {
            set_last_error(error);
            report(error == error_codes::ERROR_ACCESS_DENIED
                       ? SuspendResult::AccessDenied
                       : SuspendResult::InvalidHandle);
            return ~0u;
        }

        // A thread that has already run to completion cannot be suspended:
        // Windows fails the call (STATUS_THREAD_IS_TERMINATING) instead of
        // raising a count nobody can ever lower. A caller that suspends in a
        // loop relies on that failure to stop.
        if (gt->terminated) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            report(SuspendResult::Terminating);
            return ~0u;
        }

        // The suspend count is capped at MAXIMUM_SUSPEND_COUNT; the call that
        // would exceed it fails (STATUS_SUSPEND_COUNT_EXCEEDED) and leaves the
        // count untouched.
        if (gt->suspend_count >= MAXIMUM_SUSPEND_COUNT) {
            set_last_error(error_codes::ERROR_SIGNAL_REFUSED);
            report(SuspendResult::CountExceeded);
            return ~0u;
        }

        // CV-07: Return previous suspend count and increment.
        prev = gt->suspend_count;
        gt->suspend_count++;
        tid = gt->thread_id;
        launched = gt->launched;
    }
    report(SuspendResult::Ok);

    // A launched (running) thread must actually stop, so GetThreadContext can
    // read a consistent register snapshot. Done outside m_mutex because
    // request_suspend blocks until the target parks. A not-yet-launched
    // CREATE_SUSPENDED thread has no runner to park; its seeded GuestThread.cpu
    // already reflects the correct start context.
    if (launched) {
        ThreadSuspendRegistry::instance().request_suspend(tid);
    }
    return prev;
}

GuestThread* ThreadManager::get_thread(u32 thread_id) {
    std::lock_guard lock(m_mutex);
    auto it = m_threads.find(thread_id);
    return (it != m_threads.end()) ? it->second.get() : nullptr;
}

void ThreadManager::run_async(u32 tid, GuestAddr start_addr, GuestAddr param,
                              u64 second_param, u64 third_param,
                              usize arg_count) {
    if (m_shutdown.load(std::memory_order_acquire)) return;

    bool launched = false;
    u32 suspend_count = 0;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_threads.find(tid);
        if (it != m_threads.end()) {
            launched = it->second->launched;
            suspend_count = it->second->suspend_count;
        }
    }
    if (thread_diag_enabled()) {
        std::fprintf(
            stderr,
            "[thread-diag] pid=%d run-async self=%u target=%u start=0x%llx "
            "launched=%d suspend=%u args=%zu\n",
            static_cast<int>(::getpid()), get_current_thread_id(), tid,
            static_cast<unsigned long long>(start_addr),
            launched ? 1 : 0, suspend_count, arg_count);
    }
    if (m_run_callback) {
        m_run_callback(tid, start_addr, param, second_param, third_param,
                       arg_count);
    }
}

bool ThreadManager::start_detached_thread(GuestAddr start, GuestAddr parameter) {
    u32 tid = 0;
    const Handle handle = create_thread(0, start, parameter, 0, &tid);
    if (handle == NULL_HANDLE || handle == INVALID_HANDLE || tid == 0) return false;
    // As CreateThread: ResumeThread must not launch it a second time.
    if (GuestThread* thread = get_thread(tid)) thread->launched = true;
    run_async(tid, start, parameter);
    close_thread_handle(handle);
    return true;
}

const GuestThread* ThreadManager::get_thread(u32 thread_id) const {
    std::lock_guard lock(m_mutex);
    auto it = m_threads.find(thread_id);
    return (it != m_threads.end()) ? it->second.get() : nullptr;
}

std::vector<u32> ThreadManager::list_thread_ids() const {
    std::lock_guard lock(m_mutex);
    std::vector<u32> ids;
    ids.reserve(m_threads.size());
    for (const auto& [tid, thread] : m_threads) {
        if (thread && !thread->terminated) {
            ids.push_back(tid);
        }
    }
    return ids;
}

Handle ThreadManager::open_thread(u32 thread_id, u32 access, u32 flags) {
    std::lock_guard lock(m_mutex);
    auto it = m_threads.find(thread_id);
    if (it == m_threads.end()) {
        return NULL_HANDLE;
    }
    // Learn OpenThread: the handle is granted only the requested access
    // (thread_lifecycle_probe SUSPEND_NO_ACCESS / OPEN_INHERIT).
    Handle handle =
        m_handles.allocate(HandleType::Thread, it->second.get(), flags, access);
    if (thread_diag_enabled()) {
        std::fprintf(
            stderr,
            "[thread-diag] pid=%d open self=%u target=%u handle=0x%x "
            "access=0x%x flags=0x%x nt=0x%llx\n",
            static_cast<int>(::getpid()), get_current_thread_id(), thread_id,
            handle, access, flags,
            static_cast<unsigned long long>(it->second->nt_id));
    }
    return handle;
}

void ThreadManager::on_thread_handle_close(void* object) {
    auto* gt = static_cast<GuestThread*>(object);
    if (gt == nullptr) return;
    std::lock_guard lock(m_mutex);
    // The closing handle is already gone from the table. When nothing else
    // refers to a terminated GuestThread, destroy it — OpenThread then fails
    // with ERROR_INVALID_PARAMETER (thread_lifecycle_probe OPEN_EXITED_ALL_CLOSED).
    if (!gt->terminated) return;
    if (m_handles.count_object(gt, HandleType::Thread) != 0) return;
    const u32 tid = gt->thread_id;
    const std::uint64_t nt = gt->nt_id;
    m_thread_descriptions.erase(tid);
    m_threads.erase(tid);
    if (nt != 0) ntsync::global().destroy(nt);
}

Handle ThreadManager::duplicate_thread_handle(Handle source_handle, u32 flags,
                                              u32 access) {
    std::lock_guard lock(m_mutex);

    GuestThread* target = nullptr;
    if (source_handle == get_current_thread()) {
        auto it = m_threads.find(t_current_guest_tid);
        if (it == m_threads.end()) {
            return INVALID_HANDLE;
        }
        target = it->second.get();
    } else {
        target = m_handles.get_as<GuestThread>(source_handle, HandleType::Thread);
        if (target == nullptr) {
            return INVALID_HANDLE;
        }
    }

    Handle duplicate = m_handles.allocate(HandleType::Thread, target, flags, access);
    if (thread_diag_enabled()) {
        std::fprintf(
            stderr,
            "[thread-diag] pid=%d duplicate self=%u source=0x%x target=%u "
            "handle=0x%x nt=0x%llx\n",
            static_cast<int>(::getpid()), get_current_thread_id(), source_handle,
            target->thread_id,
            duplicate, static_cast<unsigned long long>(target->nt_id));
    }
    return duplicate;
}

bool ThreadManager::set_thread_description(Handle thread_handle,
                                           std::u16string description,
                                           u32* hresult) {
    using namespace thread_description_contract;
    std::lock_guard lock(m_mutex);
    const HandleType type = m_handles.get_type(thread_handle);
    if (thread_handle == static_cast<Handle>(-1) ||
        (thread_handle != get_current_thread() && type != HandleType::Invalid &&
         type != HandleType::Thread)) {
        if (hresult) *hresult = NT_TYPE_MISMATCH_HR;
        return false;
    }
    u32 error = 0;
    GuestThread* thread = resolve_thread_handle(
        thread_handle, THREAD_SET_LIMITED_INFORMATION, error);
    if (!thread) {
        if (hresult) *hresult = error == error_codes::ERROR_ACCESS_DENIED
            ? NT_ACCESS_DENIED_HR : NT_INVALID_HANDLE_HR;
        return false;
    }
    note_present_cpu_name(*thread, description);
    apply_host_thread_name(thread->thread_id, description);
    m_thread_descriptions[thread->thread_id] = std::move(description);
    if (hresult) *hresult = NT_SUCCESS_HR;
    return true;
}

void ThreadManager::apply_host_thread_name(u32 thread_id,
                                           const std::u16string& description) {
    if (thread_id != t_current_guest_tid) return;
    (void)name_calling_host_thread(description);
}

void ThreadManager::note_present_cpu_name(const GuestThread& thread,
                                          const std::u16string& name) {
    // [present-cpu] (VITREO_PRESENT_RATE) meters every named thread. A thread
    // naming itself passes its own port; another thread's comes from its CPU
    // accounting once it runs (bind_current_host_thread otherwise).
    if (!diagnostics::present_cpu::enabled()) return;
    unsigned int port = 0;
    if (thread.thread_id == t_current_guest_tid) {
        port = diagnostics::present_cpu::current_thread_port();
    } else if (thread.cpu_accounting && !thread.terminated) {
        port = thread.cpu_accounting->port();
    }
    diagnostics::present_cpu::note_thread_name(thread.thread_id, name, port);
}

bool ThreadManager::set_thread_description_for_id(u32 thread_id,
                                                  std::u16string description) {
    if (thread_id == 0xFFFFFFFFu) {
        thread_id = t_current_guest_tid;
    }
    std::lock_guard lock(m_mutex);
    if (diagnostics::present_cpu::enabled()) {
        if (const auto it = m_threads.find(thread_id);
            it != m_threads.end() && it->second) {
            note_present_cpu_name(*it->second, description);
        } else if (thread_id == t_current_guest_tid) {
            diagnostics::present_cpu::note_thread_name(
                thread_id, description,
                diagnostics::present_cpu::current_thread_port());
        }
    }
    apply_host_thread_name(thread_id, description);
    m_thread_descriptions[thread_id] = std::move(description);
    return true;
}

bool ThreadManager::get_thread_description(Handle thread_handle,
                                           std::u16string& description,
                                           u32* hresult) {
    using namespace thread_description_contract;
    std::lock_guard lock(m_mutex);
    const HandleType type = m_handles.get_type(thread_handle);
    if (thread_handle == static_cast<Handle>(-1) ||
        (thread_handle != get_current_thread() && type != HandleType::Invalid &&
         type != HandleType::Thread)) {
        if (hresult) *hresult = NT_TYPE_MISMATCH_HR;
        return false;
    }
    u32 error = 0;
    GuestThread* thread = resolve_thread_handle(
        thread_handle, THREAD_QUERY_LIMITED_INFORMATION, error);
    if (!thread) {
        if (hresult) *hresult = error == error_codes::ERROR_ACCESS_DENIED
            ? NT_ACCESS_DENIED_HR : NT_INVALID_HANDLE_HR;
        return false;
    }
    auto it = m_thread_descriptions.find(thread->thread_id);
    description = it == m_thread_descriptions.end() ? std::u16string{} : it->second;
    if (hresult) *hresult = NT_SUCCESS_HR;
    return true;
}

bool ThreadManager::queue_user_apc(Handle thread_handle, GuestAddr pfn,
                                   u64 parameter) {
    // Learn: QueueUserAPC fails with ERROR_GEN_FAILURE while the target is
    // terminating. A NULL pfn is queued (measured; it faults on delivery).
    (void)pfn;

    u32 thread_id = 0;
    bool terminated = false;
    {
        std::lock_guard lock(m_mutex);
        if (thread_handle == get_current_thread()) {
            thread_id = t_current_guest_tid;
            auto it = m_threads.find(thread_id);
            if (it == m_threads.end()) {
                return false;
            }
            terminated = it->second->terminated;
        } else {
            GuestThread* gt =
                m_handles.get_as<GuestThread>(thread_handle, HandleType::Thread);
            if (gt == nullptr) {
                return false;
            }
            thread_id = gt->thread_id;
            terminated = gt->terminated;
        }
        if (terminated) {
            return false;
        }
        m_user_apcs[thread_id].push_back(UserApc{pfn, parameter, m_apc_seq++});
    }
    if (m_apc_wake) m_apc_wake(thread_id);
    if (thread_diag_enabled()) {
        std::fprintf(
            stderr,
            "[thread-diag] pid=%d QueueUserAPC self=%u target=%u pfn=0x%llx "
            "data=0x%llx\n",
            static_cast<int>(::getpid()), get_current_thread_id(), thread_id,
            static_cast<unsigned long long>(pfn),
            static_cast<unsigned long long>(parameter));
    }
    return true;
}

bool ThreadManager::has_pending_user_apcs(u32 thread_id) const {
    std::lock_guard lock(m_mutex);
    auto it = m_user_apcs.find(thread_id);
    return it != m_user_apcs.end() && !it->second.empty();
}

usize ThreadManager::drain_user_apcs(u32 thread_id, const UserApcInvoke& invoke) {
    std::vector<UserApc> pending;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_user_apcs.find(thread_id);
        if (it == m_user_apcs.end() || it->second.empty()) {
            return 0;
        }
        pending.swap(it->second);
    }
    if (!invoke) {
        // Re-queue if we cannot deliver — never pretend APCs ran.
        std::lock_guard lock(m_mutex);
        auto& queue = m_user_apcs[thread_id];
        queue.insert(queue.begin(), pending.begin(), pending.end());
        return 0;
    }
    for (const auto& apc : pending) {
        invoke(apc.pfn, apc.parameter);
    }
    return pending.size();
}

bool ThreadManager::queue_io_completion_apc(GuestAddr pfn, u32 error_code,
                                            u32 bytes_transferred,
                                            GuestAddr overlapped) {
    return queue_apc3_for_thread(t_current_guest_tid, pfn, error_code,
                                 bytes_transferred, overlapped);
}

bool ThreadManager::queue_apc3_for_thread(u32 thread_id, GuestAddr pfn,
                                          u64 arg0, u64 arg1, u64 arg2) {
    if (!queue_apc3_for_thread_quiet(thread_id, pfn, arg0, arg1, arg2)) {
        return false;
    }
    // Wake any alertable wait immediately; otherwise the completion sits
    // finished-but-unnoticed until the waiter's next poll slice.
    notify_apc_queued(thread_id);
    return true;
}

bool ThreadManager::queue_apc3_for_thread_quiet(u32 thread_id, GuestAddr pfn,
                                                u64 arg0, u64 arg1, u64 arg2) {
    if (pfn == 0 || thread_id == 0) {
        return false;
    }
    std::lock_guard lock(m_mutex);
    m_io_completion_apcs[thread_id].push_back(
        IoCompletionApc{pfn, arg0, arg1, arg2, 0, 3, m_apc_seq++});
    return true;
}

void ThreadManager::notify_apc_queued(u32 thread_id) {
    if (m_apc_wake) m_apc_wake(thread_id);
}

bool ThreadManager::queue_wsa_completion_apc(u32 thread_id, GuestAddr pfn,
                                             u32 error_code,
                                             u32 bytes_transferred,
                                             GuestAddr overlapped, u32 flags) {
    if (pfn == 0 || thread_id == 0) {
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        m_io_completion_apcs[thread_id].push_back(IoCompletionApc{
            pfn, error_code, bytes_transferred, overlapped, flags, 4,
            m_apc_seq++});
    }
    if (m_apc_wake) m_apc_wake(thread_id);
    return true;
}

bool ThreadManager::has_pending_io_completion_apcs(u32 thread_id) const {
    std::lock_guard lock(m_mutex);
    auto it = m_io_completion_apcs.find(thread_id);
    return it != m_io_completion_apcs.end() && !it->second.empty();
}

usize ThreadManager::drain_io_completion_apcs(
    u32 thread_id, const IoCompletionApcInvoke& invoke) {
    std::vector<IoCompletionApc> pending;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_io_completion_apcs.find(thread_id);
        if (it == m_io_completion_apcs.end() || it->second.empty()) {
            return 0;
        }
        pending.swap(it->second);
    }
    if (!invoke) {
        std::lock_guard lock(m_mutex);
        auto& queue = m_io_completion_apcs[thread_id];
        queue.insert(queue.begin(), pending.begin(), pending.end());
        return 0;
    }
    for (const auto& apc : pending) {
        invoke(apc.pfn, apc.arg0, apc.arg1, apc.arg2, apc.arg3, apc.argc);
    }
    return pending.size();
}

bool ThreadManager::queue_nt_io_apc(GuestAddr pfn, GuestAddr context,
                                    GuestAddr io_status_block) {
    return queue_nt_io_apc_to(t_current_guest_tid, pfn, context, io_status_block);
}

bool ThreadManager::queue_nt_io_apc_to(u32 thread_id, GuestAddr pfn, GuestAddr context,
                                       GuestAddr io_status_block) {
    if (pfn == 0) {
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        m_nt_io_apcs[thread_id].push_back(
            NtIoApc{pfn, context, io_status_block, m_apc_seq++});
    }
    if (m_apc_wake) m_apc_wake(thread_id);
    return true;
}

bool ThreadManager::has_pending_nt_io_apcs(u32 thread_id) const {
    std::lock_guard lock(m_mutex);
    auto it = m_nt_io_apcs.find(thread_id);
    return it != m_nt_io_apcs.end() && !it->second.empty();
}

usize ThreadManager::drain_nt_io_apcs(u32 thread_id,
                                      const NtIoApcInvoke& invoke) {
    std::vector<NtIoApc> pending;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_nt_io_apcs.find(thread_id);
        if (it == m_nt_io_apcs.end() || it->second.empty()) {
            return 0;
        }
        pending.swap(it->second);
    }
    if (!invoke) {
        std::lock_guard lock(m_mutex);
        auto& queue = m_nt_io_apcs[thread_id];
        queue.insert(queue.begin(), pending.begin(), pending.end());
        return 0;
    }
    for (const auto& apc : pending) {
        invoke(apc.pfn, apc.context, apc.io_status_block);
    }
    return pending.size();
}

usize ThreadManager::drain_all_apcs(u32 thread_id, const UserApcInvoke& user,
                                    const IoCompletionApcInvoke& io,
                                    const NtIoApcInvoke& nt) {
    std::vector<UserApc> users;
    std::vector<IoCompletionApc> ios;
    std::vector<NtIoApc> nts;
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_user_apcs.find(thread_id); it != m_user_apcs.end())
            users.swap(it->second);
        if (auto it = m_io_completion_apcs.find(thread_id);
            it != m_io_completion_apcs.end())
            ios.swap(it->second);
        if (auto it = m_nt_io_apcs.find(thread_id); it != m_nt_io_apcs.end())
            nts.swap(it->second);
    }
    usize ui = 0, ii = 0, ni = 0;
    constexpr u64 kNone = ~u64{0};
    for (;;) {
        const u64 us = ui < users.size() ? users[ui].seq : kNone;
        const u64 is = ii < ios.size() ? ios[ii].seq : kNone;
        const u64 ns = ni < nts.size() ? nts[ni].seq : kNone;
        if (us == kNone && is == kNone && ns == kNone) break;
        if (us <= is && us <= ns) {
            const UserApc& a = users[ui++];
            if (user) user(a.pfn, a.parameter);
        } else if (is <= ns) {
            const IoCompletionApc& a = ios[ii++];
            if (io) io(a.pfn, a.arg0, a.arg1, a.arg2, a.arg3, a.argc);
        } else {
            const NtIoApc& a = nts[ni++];
            if (nt) nt(a.pfn, a.context, a.io_status_block);
        }
    }
    return users.size() + ios.size() + nts.size();
}

// =============================================================================
// Thunk Registration
// =============================================================================

DllExports create_thread_thunks(ThreadManager& mgr) {
    DllExports exports;
    exports.entries.reserve(8);

    // HANDLE CreateThread(lpSA, dwStackSize, lpStartAddress,
    //                     lpParameter, dwCreationFlags, lpThreadId)
    exports.entries.push_back(ExportEntry{
        "CreateThread", 1,
        [&mgr](Marshaler& m) {
            // SECURITY_ATTRIBUTES: only bInheritHandle is observed
            // (thread_lifecycle_probe CREATE_INHERIT). Security descriptor is
            // not applied (capsule has no NT security).
            u32 handle_flags = 0;
            const GuestAddr sa = m.arg_ptr(0);
            if (sa != 0) {
                // BOOL bInheritHandle at offset 8 (x64) / 8 (x86 after nLength+pad)
                // winnt.h: DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle
                // x64: nLength@0, pad@4, lpSD@8, bInherit@16
                // x86: nLength@0, lpSD@4, bInherit@8
                const GuestAddr inherit_off = m.cpu_is_64bit() ? sa + 16 : sa + 8;
                if (m.read_u32(inherit_off).value_or(0) != 0) {
                    handle_flags = HandleFlags::Inherit;
                }
            }
            const usize stack_size = static_cast<usize>(m.arg(1));
            GuestAddr start = m.arg(2);          // lpStartAddress
            GuestAddr param = m.arg(3);          // lpParameter
            u32 flags       = static_cast<u32>(m.arg(4));  // dwCreationFlags
            GuestAddr tid_ptr = m.arg_ptr(5);    // lpThreadId (output)

            u32 tid_val = 0;
            Handle h = mgr.create_thread(
                stack_size, start, param, flags, &tid_val, 0, handle_flags);
            if (h == NULL_HANDLE) {  // last error set by create_thread
                m.set_return(0);
                return;
            }

            if (tid_ptr != 0) {
                (void)m.write_u32(tid_ptr, tid_val);
            }

            VITREO_TRACE(
                "[thread] CreateThread: addr=0x%llx param=0x%llx flags=0x%x tid=%u -> handle=0x%x\n",
                static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(param), flags, tid_val, h);
            if (environment_flag_enabled("VITREO_WIN_DIAG") ||
                thread_diag_enabled()) {
                ipc::ipc_log(
                    "CreateThread start=0x%llx param=0x%llx flags=0x%x tid=%u -> h=0x%x",
                    static_cast<unsigned long long>(start),
                    static_cast<unsigned long long>(param), flags, tid_val, h);
            }
            // Launch the thread function asynchronously on a real host thread
            // (unless CREATE_SUSPENDED).
            if (!(flags & CREATE_SUSPENDED)) {
                VITREO_TRACE("[thread] Launching thread tid=%u via run_async\n", tid_val);
                // Mark as launched so resume_thread won't re-launch
                auto* gt = mgr.get_thread(tid_val);
                if (gt) gt->launched = true;
                mgr.run_async(tid_val, start, param);
            } else if (flags & CREATE_SUSPENDED) {
                VITREO_TRACE("[thread] Thread tid=%u created SUSPENDED\n", tid_val);
            }

            m.set_return(static_cast<u64>(h));
        },
        24
    });

    // VOID ExitThread(DWORD dwExitCode) — never returns to the caller.
    exports.entries.push_back(ExportEntry{
        "ExitThread", 2,
        [&mgr](Marshaler& m) {
            mgr.exit_current_thread_noreturn(m, static_cast<u32>(m.arg(0)));
        },
        4
    });

    // BOOL GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode)
    exports.entries.push_back(ExportEntry{
        "GetExitCodeThread", 3,
        [&mgr](Marshaler& m) {
            Handle handle = static_cast<Handle>(m.arg(0));
            GuestAddr out_ptr = m.arg_ptr(1);
            auto result = mgr.get_exit_code(handle);
            if (result.is_ok()) {
                u32 code = result.value();
                // Check if thread is still running
                auto* gt = mgr.get_thread_by_handle(handle);
                if (gt && !gt->terminated) code = 259; // STILL_ACTIVE
                if (out_ptr) (void)m.write_u32(out_ptr, code);
                m.set_return(1); // TRUE
            } else {
                m.set_return(0); // FALSE
            }
        },
        8
    });

    // BOOL TerminateThread(HANDLE hThread, DWORD dwExitCode)
    exports.entries.push_back(ExportEntry{
        "TerminateThread", 4,
        [&mgr](Marshaler& m) {
            Handle handle = static_cast<Handle>(m.arg(0));
            u32 exit_code = static_cast<u32>(m.arg(1));
            bool ok = mgr.terminate_thread(handle, exit_code);
            m.set_return(ok ? 1u : 0u);
        },
        8
    });

    // HANDLE GetCurrentThread()
    exports.entries.push_back(ExportEntry{
        "GetCurrentThread", 5,
        [](Marshaler& m) {
            m.set_return(0xFFFFFFFFFFFFFFFEULL);
        }
    });

    // DWORD GetCurrentThreadId()
    exports.entries.push_back(ExportEntry{
        "GetCurrentThreadId", 6,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.get_current_thread_id()));
        }
    });

    // DWORD ResumeThread(HANDLE hThread)
    exports.entries.push_back(ExportEntry{
        "ResumeThread", 7,
        [&mgr](Marshaler& m) {
            Handle handle = static_cast<Handle>(m.arg(0));
            u32 prev = mgr.resume_thread(handle);
            VITREO_TRACE("[thread] ResumeThread(0x%x) -> prev=%u\n", handle, prev);
            m.set_return(static_cast<u64>(prev));
        },
        4
    });

    // DWORD SuspendThread(HANDLE hThread)
    exports.entries.push_back(ExportEntry{
        "SuspendThread", 8,
        [&mgr](Marshaler& m) {
            Handle handle = static_cast<Handle>(m.arg(0));
            u32 prev = mgr.suspend_thread(handle);
            m.set_return(static_cast<u64>(prev));
        },
        4
    });

    return exports;
}

} // namespace vitreo::winapi
