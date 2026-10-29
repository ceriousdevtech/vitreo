// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_job.cpp
/// @brief Job objects: grouping processes so they can be limited and killed together.
///
/// Create/Assign/Query/SetInformation/Terminate plus the completion-port
/// notification a job owner subscribes to. The state lives in Kernel32ExtManager
/// (m_jobs, guarded by m_job_mutex); this file is the Win32 face of it.
///
/// Split out of the "Group A: Process/Job" run of kernel32_ext.cpp, which held
/// two unrelated surfaces: jobs, and the cross-process memory and thread calls
/// now in kernel32_remote.cpp. Sharing a banner is not sharing a subject.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "core/sync/ntsync.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace vitreo::winapi {

// ---- Group A: Process/Job ----
void Kernel32ExtManager::impl_CreateJobObjectA(Marshaler& m) {
    // HANDLE CreateJobObjectA(LPSECURITY_ATTRIBUTES, LPCSTR)
    // https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createjobobjecta
    // Named jobs share the CreateJobObjectW limitation (ERROR_NOT_SUPPORTED).
    // A NULL name is the same contract as the W export.
    const GuestAddr name = m.arg_ptr(1);
    if (name != 0) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }
    impl_CreateJobObjectW(m);
}

void Kernel32ExtManager::impl_CreateJobObjectW(Marshaler& m) {
    const GuestAddr attributes = m.arg_ptr(0);
    const GuestAddr name = m.arg_ptr(1);
    if (m_handle_table == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }
    if (name != 0) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }
    u32 handle_flags = 0;
    if (attributes != 0) {
        // SECURITY_ATTRIBUTES contains guest-width pointers and padding.
        // A NULL descriptor selects the same default security as NULL attrs.
        const auto descriptor = m.cpu_is_64bit()
            ? m.memory().read_u64(attributes + 8)
            : m.memory().read_u32(attributes + 4).map([](u32 v) { return u64(v); });
        const auto inherit = m.memory().read_u32(
            attributes + (m.cpu_is_64bit() ? 16 : 8));
        if (descriptor.is_err() || inherit.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        if (descriptor.value() != 0) {
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(0);
            return;
        }
        if (inherit.value() != 0) handle_flags = HandleFlags::Inherit;
    }
    auto job = std::make_shared<JobState>();
    job->extended_limits.assign(m.cpu_is_64bit() ? 144u : 112u, 0);
    const Handle handle =
        m_handle_table->allocate(HandleType::Job, nullptr, handle_flags);
    {
        std::lock_guard lock(m_job_mutex);
        m_jobs[handle] = std::move(job);
    }
    ipc::ipc_log("CreateJobObjectW -> h=0x%x",
                 static_cast<unsigned>(handle));
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(handle);
}
void Kernel32ExtManager::impl_AssignProcessToJobObject(Marshaler& m) {
    // BOOL AssignProcessToJobObject(HANDLE hJob, HANDLE hProcess)
    const Handle job_handle = static_cast<Handle>(m.arg(0));
    const Handle process_handle = static_cast<Handle>(m.arg(1));
    std::shared_ptr<JobState> job;
    {
        std::lock_guard lock(m_job_mutex);
        const auto found = m_jobs.find(job_handle);
        if (found != m_jobs.end()) job = found->second;
    }
    if (!job || !add_job_member(job, process_handle)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    ipc::ipc_log(
        "AssignProcessToJobObject job=0x%x process=0x%x -> TRUE",
        static_cast<unsigned>(job_handle),
        static_cast<unsigned>(process_handle));
    m.set_return(1);
}

pid_t Kernel32ExtManager::pid_for_process_handle(Handle process_handle) const {
    if (process_handle == CURRENT_PROCESS_PSEUDO) return ::getpid();
    if (m_handle_table == nullptr ||
        m_handle_table->get_type(process_handle) != HandleType::Process) {
        return 0;
    }
    auto* process = m_handle_table->get_as<ipc::ProcessObject>(
        process_handle, HandleType::Process);
    return process != nullptr && process->pid > 0 ? process->pid : 0;
}

bool Kernel32ExtManager::add_job_member(const std::shared_ptr<JobState>& job,
                                        Handle process_handle) {
    const pid_t pid = pid_for_process_handle(process_handle);
    if (pid <= 0) return false;
    ipc::ProcessObject* process = nullptr;
    if (pid != ::getpid()) {
        process = m_handle_table->get_as<ipc::ProcessObject>(
            process_handle, HandleType::Process);
    }
    const bool already_exited =
        process != nullptr && process->exited.load(std::memory_order_acquire);
    {
        std::lock_guard lock(m_job_mutex);
        for (const auto& member : job->members) {
            if (member.pid == pid) return true;  // already a member
        }
        job->members.push_back(JobMember{pid, process, already_exited, false});
    }
    notify_job_new_process(job, pid);
    if (process == nullptr || already_exited) {
        return true;
    }
    // Watch the member so the job's completion port hears about its exit
    // (JOB_OBJECT_MSG_EXIT_PROCESS / ACTIVE_PROCESS_ZERO, s4_job_probe).
    std::weak_ptr<JobState> weak = job;
    const auto cancel = job->watch_cancel;
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread watcher([this, weak, process, pid, cancel, done] {
        const u32 waiter_tid = 0xE8000000u | (static_cast<u32>(pid) & 0x07FFFFFFu);
        ThreadManager::set_current_thread_id(waiter_tid);
        const u64 exit_event = process->exit_event;
        if (exit_event != 0) {
            (void)ntsync::global().wait(&exit_event, 1, false,
                                        ntsync::INFINITE_TIMEOUT, waiter_tid,
                                        cancel.get());
        }
        if (!cancel->load(std::memory_order_acquire)) {
            if (auto job_state = weak.lock()) {
                on_job_member_exit(job_state, pid,
                                   process->exit_code.load(std::memory_order_acquire));
            }
        }
        done->store(true, std::memory_order_release);
    });
    std::vector<std::thread> finished;
    {
        std::lock_guard lock(m_job_watchers_mutex);
        for (auto it = m_job_watchers.begin(); it != m_job_watchers.end();) {
            if (it->done->load(std::memory_order_acquire)) {
                finished.push_back(std::move(it->thread));
                it = m_job_watchers.erase(it);
            } else {
                ++it;
            }
        }
        m_job_watchers.push_back(JobWatcher{std::move(watcher), done});
    }
    for (auto& t : finished) {
        if (t.joinable()) t.join();
    }
    return true;
}

void Kernel32ExtManager::on_job_member_exit(const std::shared_ptr<JobState>& job,
                                            pid_t pid, u32 exit_code) {
    u32 port = 0;
    u64 key = 0;
    bool post_exit = false;
    bool post_zero = false;
    {
        std::lock_guard lock(m_job_mutex);
        JobMember* member = nullptr;
        for (auto& candidate : job->members) {
            if (candidate.pid == pid) member = &candidate;
        }
        if (member == nullptr || member->exited) return;
        member->exited = true;
        // A member ended by TerminateJobObject or job close gets no exit
        // message of its own (s4_job_probe after_terminate: only msg 4).
        post_exit = !member->killed;
        u32 active = 0;
        for (const auto& candidate : job->members) {
            if (!candidate.exited) ++active;
        }
        post_zero = active == 0;
        port = job->completion_port;
        key = job->completion_key;
    }
    if (port == 0) return;
    // winnt.h JOB_OBJECT_MSG_*: EXIT_PROCESS 7, ABNORMAL_EXIT_PROCESS 8 (an
    // exit code that is an error NTSTATUS, e.g. an unhandled exception),
    // ACTIVE_PROCESS_ZERO 4 (lpOverlapped 0).
    constexpr u32 kMsgActiveProcessZero = 4;
    constexpr u32 kMsgExitProcess = 7;
    constexpr u32 kMsgAbnormalExitProcess = 8;
    if (post_exit) {
        const u32 msg = (exit_code & 0xC0000000u) == 0xC0000000u
            ? kMsgAbnormalExitProcess : kMsgExitProcess;
        (void)enqueue_completion_packet(
            port, CompletionEntry{msg, key, static_cast<u64>(pid), 0, 0});
    }
    if (post_zero) {
        (void)enqueue_completion_packet(
            port, CompletionEntry{kMsgActiveProcessZero, key, 0, 0, 0});
    }
}

void Kernel32ExtManager::impl_IsProcessInJob(Marshaler& m) {
    // BOOL IsProcessInJob(HANDLE ProcessHandle, HANDLE JobHandle, PBOOL Result)
    // Learn: JobHandle NULL tests membership in any job; otherwise that job.
    const Handle process_handle = static_cast<Handle>(m.arg(0));
    const Handle job_handle = static_cast<Handle>(m.arg(1));
    const GuestAddr result_ptr = m.arg_ptr(2);
    if (result_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const bool valid_process =
        process_handle == CURRENT_PROCESS_PSEUDO ||
        (m_handle_table != nullptr &&
         m_handle_table->get_type(process_handle) == HandleType::Process);
    if (!valid_process) {
        (void)m.memory().write_u32(result_ptr, 0);
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    const pid_t pid = pid_for_process_handle(process_handle);
    auto contains = [pid](const JobState& job) {
        for (const auto& member : job.members) {
            if (member.pid == pid) return true;
        }
        return false;
    };
    bool in_job = false;
    {
        std::lock_guard lock(m_job_mutex);
        if (job_handle == 0) {
            for (const auto& [handle, job] : m_jobs) {
                (void)handle;
                if (job && contains(*job)) {
                    in_job = true;
                    break;
                }
            }
        } else {
            const auto found = m_jobs.find(job_handle);
            if (found == m_jobs.end() || !found->second) {
                (void)m.memory().write_u32(result_ptr, 0);
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            in_job = contains(*found->second);
        }
    }
    (void)m.memory().write_u32(result_ptr, in_job ? 1u : 0u);
    m.set_return(1);
}

void Kernel32ExtManager::impl_QueryInformationJobObject(Marshaler& m) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u32 info_class = static_cast<u32>(m.arg(1));
    const GuestAddr info = m.arg_ptr(2);
    const u32 len = static_cast<u32>(m.arg(3));
    const GuestAddr ret_len = m.arg_ptr(4);
    std::shared_ptr<JobState> job;
    {
        std::lock_guard lock(m_job_mutex);
        const auto found = m_jobs.find(handle);
        if (found != m_jobs.end()) job = found->second;
    }
    if (!job) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    std::vector<u8> value;
    if (info_class == 9) { // JobObjectExtendedLimitInformation
        value = job->extended_limits;
    } else if (info_class == 4) { // JobObjectBasicUIRestrictions
        value.resize(4);
        std::memcpy(value.data(), &job->ui_restrictions, sizeof(u32));
    } else {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }
    if (ret_len != 0) {
        (void)m.write_u32(ret_len, static_cast<u32>(value.size()));
    }
    if (info == 0 || len < value.size() ||
        m.memory().write_bytes(info, value.data(), value.size()).is_err()) {
        set_last_error(info == 0 || len < value.size()
            ? error_codes::ERROR_BAD_LENGTH
            : error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    ipc::ipc_log(
        "QueryInformationJobObject h=0x%x class=%u -> TRUE len=%zu",
        static_cast<unsigned>(handle), info_class, value.size());
    m.set_return(1);
}
void Kernel32ExtManager::impl_SetInformationJobObject(Marshaler& m) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u32 info_class = static_cast<u32>(m.arg(1));
    const GuestAddr info = m.arg_ptr(2);
    const u32 len = static_cast<u32>(m.arg(3));
    std::shared_ptr<JobState> job;
    {
        std::lock_guard lock(m_job_mutex);
        const auto found = m_jobs.find(handle);
        if (found != m_jobs.end()) job = found->second;
    }
    if (!job) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    constexpr u32 JobObjectAssociateCompletionPortInformation = 7;
    const u32 required =
        info_class == 9 ? (m.cpu_is_64bit() ? 144u : 112u)
        : info_class == 4 ? 4u
        : info_class == JobObjectAssociateCompletionPortInformation
            ? (m.cpu_is_64bit() ? 16u : 8u)
            : 0u;
    if (required == 0) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }
    std::vector<u8> value(required);
    if (info == 0 || len < required ||
        m.memory().read_bytes(info, value.data(), value.size()).is_err()) {
        set_last_error(info == 0 || len < required
            ? error_codes::ERROR_BAD_LENGTH
            : error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    std::vector<pid_t> existing_processes;
    if (info_class == JobObjectAssociateCompletionPortInformation) {
        const u64 completion_key = m.cpu_is_64bit()
            ? m.memory().read_u64(info).value_or(0)
            : m.memory().read_u32(info).value_or(0);
        const u32 completion_port = static_cast<u32>(
            m.cpu_is_64bit()
                ? m.memory().read_u64(info + 8).value_or(0)
                : m.memory().read_u32(info + 4).value_or(0));
        {
            std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
            if (completion_port == 0 ||
                !m_iocp_ports.contains(completion_port)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
        }
        {
            std::lock_guard lock(m_job_mutex);
            if (job->completion_port != 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            job->completion_port = completion_port;
            job->completion_key = completion_key;
            for (const auto& member : job->members) {
                existing_processes.push_back(member.pid);
            }
        }
    } else {
        std::lock_guard lock(m_job_mutex);
        if (info_class == 9) {
            job->extended_limits = std::move(value);
        } else {
            std::memcpy(&job->ui_restrictions, value.data(), sizeof(u32));
        }
    }
    for (const pid_t pid : existing_processes) {
        notify_job_new_process(job, pid);
    }
    ipc::ipc_log(
        "SetInformationJobObject h=0x%x class=%u len=%u -> TRUE",
        static_cast<unsigned>(handle), info_class, required);
    m.set_return(1);
}

void Kernel32ExtManager::notify_job_new_process(
    const std::shared_ptr<JobState>& job, pid_t pid) {
    if (!job || pid <= 0) return;
    u32 port_handle = 0;
    u64 completion_key = 0;
    {
        std::lock_guard lock(m_job_mutex);
        if (job->completion_port == 0 || job->notified_processes.contains(pid)) {
            return;
        }
        port_handle = job->completion_port;
        completion_key = job->completion_key;
        job->notified_processes.insert(pid);
    }
    constexpr u32 JOB_OBJECT_MSG_NEW_PROCESS = 6;
    if (!enqueue_completion_packet(
            port_handle,
            CompletionEntry{JOB_OBJECT_MSG_NEW_PROCESS, completion_key,
                            static_cast<u64>(pid), 0, 0})) {
        std::lock_guard lock(m_job_mutex);
        job->notified_processes.erase(pid);
    }
}

bool Kernel32ExtManager::job_kills_on_close(const JobState& job) const {
    // JOBOBJECT_EXTENDED_LIMIT_INFORMATION begins with the basic limits;
    // LimitFlags is at offset 16 on both bitnesses (two LARGE_INTEGERs).
    constexpr u32 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
    if (job.extended_limits.size() < 20) return false;
    u32 flags = 0;
    std::memcpy(&flags, job.extended_limits.data() + 16, sizeof(flags));
    return (flags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
}

void Kernel32ExtManager::terminate_job_members(const std::shared_ptr<JobState>& job,
                                               u32 exit_code, bool include_self) {
    std::vector<ipc::ProcessObject*> targets;
    bool self_member = false;
    {
        std::lock_guard lock(m_job_mutex);
        for (auto& member : job->members) {
            if (member.exited) continue;
            if (member.process == nullptr) {
                self_member = true;
                continue;
            }
            member.killed = true;
            targets.push_back(member.process);
        }
    }
    for (auto* process : targets) {
        (void)ipc::terminate_process(*process, exit_code);
    }
    if (include_self && self_member && m_process_manager != nullptr) {
        m_process_manager->exit_process(exit_code);
    }
}

void Kernel32ExtManager::impl_TerminateJobObject(Marshaler& m) {
    // BOOL TerminateJobObject(HANDLE hJob, UINT uExitCode): every process in
    // the job ends with uExitCode (s4_job_probe terminate_job code=55); the
    // owner's port then gets ACTIVE_PROCESS_ZERO and no per-process exit
    // message.
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u32 exit_code = m.arg32(1);
    std::shared_ptr<JobState> job;
    {
        std::lock_guard lock(m_job_mutex);
        const auto found = m_jobs.find(handle);
        if (found != m_jobs.end()) job = found->second;
    }
    if (!job) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    terminate_job_members(job, exit_code, /*include_self=*/true);
    m.set_return(1);
}

bool Kernel32ExtManager::on_job_handle_close(Handle handle) {
    std::shared_ptr<JobState> job;
    bool last = false;
    {
        std::lock_guard lock(m_job_mutex);
        const auto found = m_jobs.find(handle);
        if (found == m_jobs.end()) return true;
        job = std::move(found->second);
        m_jobs.erase(found);
        last = true;
        for (const auto& [other, state] : m_jobs) {
            (void)other;
            if (state == job) {
                last = false;
                break;
            }
        }
    }
    if (!last || !job) return true;
    // The job object goes away with its last handle (a duplicate keeps it:
    // kill_on_close_first_handle). JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE then
    // ends every member with exit code 0 (kill_on_close_last_handle code=0).
    if (job_kills_on_close(*job)) {
        terminate_job_members(job, 0, /*include_self=*/false);
    }
    job->watch_cancel->store(true, std::memory_order_release);
    ntsync::global().notify_waiters();
    return true;
}

} // namespace vitreo::winapi
