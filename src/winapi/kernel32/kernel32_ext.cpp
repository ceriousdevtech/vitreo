// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext.cpp
/// @brief Extended kernel32 API stubs for common Win32 applications

#include "winapi/kernel32/system/processor_topology.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "core/sync/ntsync.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/common/thread_description_contract.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <cstdio>
#include <cstdarg>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <cwctype>
#include <algorithm>
#include <vector>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>


namespace vitreo::winapi {

void Kernel32ExtManager::set_file_manager(FileManager* mgr) {
    if (m_file_manager != nullptr && m_file_manager != mgr) {
        m_file_manager->shutdown_overlapped_pipe_io();
        m_file_manager->clear_overlapped_completion_callback();
        m_file_manager->clear_overlapped_trace_callback();
        m_file_manager->clear_io_completion_routine_queue();
    }
    m_file_manager = mgr;
    if (m_file_manager) {
        m_file_manager->set_io_completion_routine_queue(
            [this](GuestAddr pfn, u32 error_code, u32 bytes,
                   GuestAddr overlapped) {
                if (m_thread_manager) {
                    (void)m_thread_manager->queue_io_completion_apc(
                        pfn, error_code, bytes, overlapped);
                }
            });
        m_file_manager->set_overlapped_completion_callback(
            [this](Handle file_handle, u32 bytes_transferred,
                   GuestAddr overlapped, bool is_64bit,
                   u32 nt_status, u32 win_error,
                   bool completed_synchronously) {
                complete_overlapped_file_io(file_handle, bytes_transferred,
                                            overlapped, is_64bit,
                                            nt_status, win_error,
                                            completed_synchronously);
            });
        m_file_manager->set_overlapped_trace_callback(
            [this](const OverlappedIoTraceEvent& event) {
                // Completion routing belongs to the request, not the lifetime
                // of the handle used to submit it. CloseHandle may cancel an
                // outstanding read before its completion packet is posted.
                if (event.stage == OverlappedIoTraceStage::Submitted) {
                    snapshot_completion_route(event.handle, event.overlapped);
                    // Learn (ReadFile/WriteFile, OVERLAPPED): the call resets
                    // hEvent when the operation begins; a stale signal would
                    // wake a waiter while Internal is still STATUS_PENDING.
                    const GuestAddr event_field =
                        event.overlapped + (event.is_64bit ? 24u : 16u);
                    const u64 event_value = event.is_64bit
                        ? m_memory.read_u64(event_field).value_or(0)
                        : m_memory.read_u32(event_field).value_or(0);
                    if (event_value != 0 && m_sync_manager) {
                        (void)m_sync_manager->reset_event(
                            static_cast<Handle>(event_value & ~u64{1}));
                    }
                } else if (event.stage == OverlappedIoTraceStage::Failed) {
                    std::lock_guard lock(m_iocp_registry_mutex);
                    m_pending_file_completions.erase(event.overlapped);
                }
                record_overlapped_trace(event);
            });
    }
}

void Kernel32ExtManager::snapshot_completion_route(Handle handle,
                                                   GuestAddr overlapped) {
    std::lock_guard lock(m_iocp_registry_mutex);
    m_pending_file_completions.erase(overlapped);
    auto association = m_iocp_file_associations.find(handle);
    if (association == m_iocp_file_associations.end()) return;
    auto port = m_iocp_ports.find(association->second.port_handle);
    if (port == m_iocp_ports.end()) return;
    const auto modes = m_file_completion_modes.find(handle);
    m_pending_file_completions[overlapped] = {
        port->second, association->second.port_handle,
        association->second.completion_key,
        modes == m_file_completion_modes.end() ? u8{0} : modes->second};
}

[[nodiscard]] u32 host_logical_processor_count() noexcept {
    return processor_topology::processor_count();
}

namespace {

std::optional<ipc::GuestProcessMetadata> guest_process_metadata(
    const std::string& host_path) {
    std::ifstream input(host_path, std::ios::binary);
    if (!input) return std::nullopt;
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end <= 0) return std::nullopt;
    input.seekg(0, std::ios::beg);
    std::vector<u8> bytes(static_cast<usize>(end));
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!input) return std::nullopt;
    auto parsed = pe::parse_pe(bytes);
    if (parsed.is_err()) return std::nullopt;
    return ipc::GuestProcessMetadata{
        parsed.value().is_64bit,
        pe::PRIMARY_PEB_ADDRESS,
        parsed.value().image_base,
        true,
    };
}




struct IocpDiagConfig {
    bool enabled{false};
    std::optional<u32> pid;
    std::optional<u64> object_id;
    std::optional<GuestAddr> overlapped;
};

const IocpDiagConfig& iocp_diag_config() {
    static const IocpDiagConfig config = [] {
        IocpDiagConfig result;
        const char* enabled = std::getenv("VITREO_IOCP_DIAG");
        result.enabled =
            enabled != nullptr && enabled[0] != '\0' && enabled[0] != '0';
        auto parse = [](const char* name) -> std::optional<u64> {
            const char* value = std::getenv(name);
            if (value == nullptr || value[0] == '\0') return std::nullopt;
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(value, &end, 0);
            return end != value && *end == '\0'
                ? std::optional<u64>{static_cast<u64>(parsed)}
                : std::nullopt;
        };
        if (auto pid = parse("VITREO_IOCP_DIAG_PID")) {
            result.pid = static_cast<u32>(*pid);
        }
        result.object_id = parse("VITREO_IOCP_DIAG_OBJECT");
        result.overlapped = parse("VITREO_IOCP_DIAG_OV");
        return result;
    }();
    return config;
}

bool iocp_diag_matches(GuestAddr overlapped, u64 object_id = 0) {
    const auto& config = iocp_diag_config();
    if (!config.enabled) return false;
    if (config.pid && *config.pid != static_cast<u32>(::getpid())) return false;
    if (config.object_id && *config.object_id != object_id) return false;
    if (config.overlapped && *config.overlapped != overlapped) return false;
    return true;
}

const char* iocp_stage_name(OverlappedIoTraceStage stage) {
    switch (stage) {
        case OverlappedIoTraceStage::Submitted: return "Submitted";
        case OverlappedIoTraceStage::DataAvailable: return "DataAvailable";
        case OverlappedIoTraceStage::BufferFilled: return "BufferFilled";
        case OverlappedIoTraceStage::CompletionPrepared:
            return "CompletionPrepared";
        case OverlappedIoTraceStage::PacketQueued: return "PacketQueued";
        case OverlappedIoTraceStage::PacketDequeued: return "PacketDequeued";
        case OverlappedIoTraceStage::GuestCallbackObserved:
            return "GuestCallbackObserved";
        case OverlappedIoTraceStage::Cancelled: return "Cancelled";
        case OverlappedIoTraceStage::Failed: return "Failed";
    }
    return "?";
}

u64 iocp_diag_elapsed_us() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}


} // namespace

// Out of the anonymous namespace so filesystem/file_io_ext.cpp can trace
// through the same locked, timestamped stderr channel; declared in
// kernel32_ext_internal.hpp.
void iocp_diag_log(const char* format, ...) {
    if (!iocp_diag_config().enabled) return;
    // Child processes inherit the same open file description for stderr, so
    // flock does not serialize their separate stdio calls. Emit the prefix,
    // event, and newline together; otherwise one process's event can appear
    // under another process's PID and manufacture duplicate completions.
    std::array<char, 2048> line{};
    const int prefix = std::snprintf(line.data(), line.size(),
        "[iocp-diag t=%llu pid=%d] ",
        static_cast<unsigned long long>(iocp_diag_elapsed_us()),
        static_cast<int>(::getpid()));
    if (prefix < 0 || static_cast<usize>(prefix) >= line.size() - 1) return;
    va_list args;
    va_start(args, format);
    const int body = std::vsnprintf(line.data() + prefix,
        line.size() - static_cast<usize>(prefix) - 1, format, args);
    va_end(args);
    if (body < 0) return;
    const usize length = std::min(line.size() - 2,
        static_cast<usize>(prefix) + static_cast<usize>(body));
    line[length] = '\n';
    (void)::write(::fileno(stderr), line.data(), length + 1);
}

// Out of the anonymous namespace because kernel32_environment.cpp's path APIs
// call it; declared in kernel32_ext_internal.hpp.
std::string normalize_dir_path(std::string path) {
    for (char& c : path) {
        if (c == '/') c = '\\';
    }
    if (path.empty()) {
        return path;
    }
    if (path.back() != '\\') {
        path.push_back('\\');
    }
    return path;
}

// =============================================================================
// Construction
// =============================================================================

Kernel32ExtManager::Kernel32ExtManager(GuestMemory& memory, capsule::Vfs& vfs)
    : m_memory(memory)
    , m_vfs(vfs)
{
    // CV-12: Generate a random 32-bit cookie for EncodePointer/DecodePointer.
    std::random_device rd;
    m_pointer_cookie = rd();
    if (m_pointer_cookie == 0) {
        m_pointer_cookie = 0xDEADBEEF; // Avoid zero cookie (identity)
    }

    const char* restricted = std::getenv("VITREO_CHILD_PROCESS_RESTRICTED");
    m_child_process_restricted =
        restricted != nullptr && std::strcmp(restricted, "1") == 0;
    const char* mitigations =
        std::getenv("VITREO_PROCESS_MITIGATION_POLICY");
    if (mitigations != nullptr) {
        unsigned long long policy0 = 0;
        unsigned long long policy1 = 0;
        if (std::sscanf(mitigations, "%llx:%llx",
                        &policy0, &policy1) == 2) {
            m_process_creation_mitigation_policy[0] =
                static_cast<u64>(policy0);
            m_process_creation_mitigation_policy[1] =
                static_cast<u64>(policy1);
        }
    }
}

void Kernel32ExtManager::record_overlapped_trace(
    const OverlappedIoTraceEvent& event) {
    if (!iocp_diag_config().enabled || event.overlapped == 0) return;

    TrackedOverlappedIo snapshot{};
    bool callback_after_ping = false;
    {
        std::lock_guard trace_lock(m_overlapped_trace_mutex);
        auto& trace = m_overlapped_traces[event.overlapped];
        if (event.stage == OverlappedIoTraceStage::Submitted) {
            callback_after_ping =
                trace.contains_ping &&
                trace.stage == OverlappedIoTraceStage::PacketDequeued;
            trace = {};
            trace.process_id = static_cast<u32>(::getpid());
            trace.submitting_thread_id =
                ThreadManager::get_current_thread_id_static();
            trace.submitted_handle = event.handle;
            trace.object_id = event.object_id;
            trace.overlapped = event.overlapped;
            trace.buffer = event.buffer;
            trace.requested_bytes = event.requested_bytes;
            trace.stage = event.stage;
            const GuestAddr event_field =
                event.overlapped + (event.is_64bit ? 24u : 16u);
            trace.event_handle = event.is_64bit
                ? m_memory.read_u64(event_field).value_or(0)
                : m_memory.read_u32(event_field).value_or(0);
        } else {
            trace.stage = event.stage;
            if (trace.process_id == 0) {
                trace.process_id = static_cast<u32>(::getpid());
                trace.submitted_handle = event.handle;
                trace.object_id = event.object_id;
                trace.overlapped = event.overlapped;
                trace.buffer = event.buffer;
                trace.requested_bytes = event.requested_bytes;
            }
        }
        trace.transferred_bytes = event.transferred_bytes;
        trace.win32_error = event.win32_error;
        trace.nt_status = event.nt_status;
        trace.contains_ping = trace.contains_ping || event.contains_ping;
        snapshot = trace;
    }

    if (event.stage == OverlappedIoTraceStage::PacketDequeued &&
        snapshot.contains_ping) {
        vitreo::diagnostics::set_trace_milestone_reached(true);
        if (m_file_mapping_manager != nullptr) {
            m_file_mapping_manager->dump_ipcz_router_link_state_candidates(
                "PacketDequeued-Ping");
        }
    }

    if (callback_after_ping) {
        iocp_diag_log(
            "stage=%s tid=%u h=0x%x object=0x%llx ov=0x%llx "
            "buffer=0x%llx requested=%u",
            iocp_stage_name(OverlappedIoTraceStage::GuestCallbackObserved),
            snapshot.submitting_thread_id,
            static_cast<unsigned>(event.handle),
            static_cast<unsigned long long>(event.object_id),
            static_cast<unsigned long long>(event.overlapped),
            static_cast<unsigned long long>(event.buffer),
            event.requested_bytes);
        if (m_file_mapping_manager != nullptr) {
            m_file_mapping_manager->dump_ipcz_router_link_state_candidates(
                "GuestCallbackObserved");
        }
        if (std::getenv("VITREO_IOCP_DIAG_DUMP_PING_CALLBACK") != nullptr) {
            vitreo::diagnostics::dump_hostcalls("iocp-ping-callback-observed");
        }
    }

    if (event.stage == OverlappedIoTraceStage::Submitted) {
        std::lock_guard registry_lock(m_iocp_registry_mutex);
        const auto association =
            m_iocp_file_associations.find(event.handle);
        if (association != m_iocp_file_associations.end()) {
            snapshot.association_found = true;
            snapshot.completion_port = association->second.port_handle;
            snapshot.completion_key = association->second.completion_key;
        }
        if (const auto modes = m_file_completion_modes.find(event.handle);
            modes != m_file_completion_modes.end()) {
            snapshot.completion_modes = modes->second;
        }
        {
            std::lock_guard trace_lock(m_overlapped_trace_mutex);
            auto found = m_overlapped_traces.find(event.overlapped);
            if (found != m_overlapped_traces.end()) {
                found->second.association_found = snapshot.association_found;
                found->second.completion_port = snapshot.completion_port;
                found->second.completion_key = snapshot.completion_key;
                found->second.completion_modes = snapshot.completion_modes;
            }
        }
    }

    if (!iocp_diag_matches(event.overlapped, event.object_id)) return;
    char preview[3 * 64 + 1]{};
    usize preview_offset = 0;
    for (u8 index = 0; index < event.preview_size; ++index) {
        const int written = std::snprintf(
            preview + preview_offset, sizeof(preview) - preview_offset,
            "%02x%s", event.preview[index],
            index + 1 == event.preview_size ? "" : " ");
        if (written <= 0) break;
        preview_offset += static_cast<usize>(written);
    }
    iocp_diag_log(
        "stage=%s tid=%u h=0x%x object=0x%llx ov=0x%llx "
        "buffer=0x%llx requested=%u transferred=%u assoc=%s "
        "port=0x%x key=0x%llx modes=0x%x event=0x%llx gle=%u "
        "status=0x%x ping=%u preview=%s",
        iocp_stage_name(event.stage), snapshot.submitting_thread_id,
        static_cast<unsigned>(event.handle),
        static_cast<unsigned long long>(event.object_id),
        static_cast<unsigned long long>(event.overlapped),
        static_cast<unsigned long long>(event.buffer),
        event.requested_bytes, event.transferred_bytes,
        snapshot.association_found ? "found" : "missing",
        snapshot.completion_port,
        static_cast<unsigned long long>(snapshot.completion_key),
        snapshot.completion_modes,
        static_cast<unsigned long long>(snapshot.event_handle),
        event.win32_error, event.nt_status, event.contains_ping ? 1u : 0u,
        event.preview_size == 0 ? "-" : preview);
}

void Kernel32ExtManager::record_completion_trace(
    OverlappedIoTraceStage stage, Handle file_handle,
    GuestAddr overlapped, u32 bytes_transferred, u32 port_handle,
    u64 completion_key, u64 event_handle, u8 completion_modes,
    bool association_found, u32 nt_status, u32 win32_error) {
    if (!iocp_diag_config().enabled || overlapped == 0) return;
    TrackedOverlappedIo snapshot{};
    {
        std::lock_guard lock(m_overlapped_trace_mutex);
        auto& trace = m_overlapped_traces[overlapped];
        if (trace.process_id == 0) {
            trace.process_id = static_cast<u32>(::getpid());
            trace.submitted_handle = file_handle;
            trace.overlapped = overlapped;
        }
        trace.stage = stage;
        trace.transferred_bytes = bytes_transferred;
        trace.completion_port = port_handle;
        trace.completion_key = completion_key;
        if (stage != OverlappedIoTraceStage::PacketDequeued) {
            trace.event_handle = event_handle;
            trace.completion_modes = completion_modes;
        }
        trace.association_found = association_found;
        trace.nt_status = nt_status;
        trace.win32_error = win32_error;
        snapshot = trace;
    }
    if (stage == OverlappedIoTraceStage::PacketDequeued &&
        snapshot.contains_ping) {
        vitreo::diagnostics::set_trace_milestone_reached(true);
        if (m_file_mapping_manager != nullptr) {
            m_file_mapping_manager->dump_ipcz_router_link_state_candidates(
                "PacketDequeued-Ping");
        }
    }
    if (!iocp_diag_matches(overlapped, snapshot.object_id)) return;
    iocp_diag_log(
        "stage=%s tid=%u h=0x%x object=0x%llx ov=0x%llx "
        "requested=%u transferred=%u assoc=%s port=0x%x key=0x%llx "
        "modes=0x%x event=0x%llx gle=%u status=0x%x ping=%u",
        iocp_stage_name(stage), snapshot.submitting_thread_id,
        static_cast<unsigned>(snapshot.submitted_handle),
        static_cast<unsigned long long>(snapshot.object_id),
        static_cast<unsigned long long>(overlapped),
        snapshot.requested_bytes, bytes_transferred,
        association_found ? "found" : "missing", port_handle,
        static_cast<unsigned long long>(completion_key),
        snapshot.completion_modes,
        static_cast<unsigned long long>(snapshot.event_handle),
        win32_error, nt_status,
        snapshot.contains_ping ? 1u : 0u);
}

void Kernel32ExtManager::dump_stuck_overlapped_traces() {
    if (!iocp_diag_config().enabled) return;
    std::vector<TrackedOverlappedIo> stuck;
    {
        std::lock_guard lock(m_overlapped_trace_mutex);
        for (const auto& [overlapped, trace] : m_overlapped_traces) {
            (void)overlapped;
            if (trace.stage == OverlappedIoTraceStage::BufferFilled ||
                trace.stage == OverlappedIoTraceStage::CompletionPrepared ||
                trace.stage == OverlappedIoTraceStage::PacketQueued) {
                stuck.push_back(trace);
            }
        }
    }
    for (const auto& trace : stuck) {
        if (!iocp_diag_matches(trace.overlapped, trace.object_id)) continue;
        iocp_diag_log(
            "[IOCP-STUCK] stage=%s tid=%u h=0x%x object=0x%llx "
            "ov=0x%llx buffer=0x%llx requested=%u transferred=%u "
            "assoc=%s port=0x%x key=0x%llx ping=%u",
            iocp_stage_name(trace.stage), trace.submitting_thread_id,
            static_cast<unsigned>(trace.submitted_handle),
            static_cast<unsigned long long>(trace.object_id),
            static_cast<unsigned long long>(trace.overlapped),
            static_cast<unsigned long long>(trace.buffer),
            trace.requested_bytes, trace.transferred_bytes,
            trace.association_found ? "found" : "missing",
            trace.completion_port,
            static_cast<unsigned long long>(trace.completion_key),
            trace.contains_ping ? 1u : 0u);
    }
}

Kernel32ExtManager::~Kernel32ExtManager() {
    {
        std::lock_guard lock(m_job_mutex);
        for (const auto& [handle, job] : m_jobs) {
            (void)handle;
            if (job) job->watch_cancel->store(true, std::memory_order_release);
        }
    }
    ntsync::global().notify_waiters();
    std::vector<JobWatcher> watchers;
    {
        std::lock_guard lock(m_job_watchers_mutex);
        watchers.swap(m_job_watchers);
    }
    for (auto& w : watchers) {
        if (w.thread.joinable()) w.thread.join();
    }
}

void Kernel32ExtManager::begin_shutdown() {
    dump_stuck_overlapped_traces();
    m_shutdown.store(true, std::memory_order_release);
    stop_all_change_notification_watches();

    // The process's handles all close as it exits, so every job it owns loses
    // its last handle: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE members end now
    // (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE members end now, so a child in
    // that job does not outlive the process; a released worker would
    // otherwise keep running).
    {
        std::vector<std::shared_ptr<JobState>> jobs;
        {
            std::lock_guard lock(m_job_mutex);
            for (const auto& [handle, job] : m_jobs) {
                (void)handle;
                if (job && std::find(jobs.begin(), jobs.end(), job) == jobs.end()) {
                    jobs.push_back(job);
                }
            }
        }
        for (const auto& job : jobs) {
            if (job_kills_on_close(*job)) {
                terminate_job_members(job, 0, /*include_self=*/false);
            }
            job->watch_cancel->store(true, std::memory_order_release);
        }
        if (!jobs.empty()) ntsync::global().notify_waiters();
    }

    std::vector<std::shared_ptr<RegisteredWaitState>> registered_waits;
    {
        std::lock_guard lock(m_registered_wait_mutex);
        registered_waits.reserve(m_registered_waits.size());
        for (auto& [handle, state] : m_registered_waits) {
            (void)handle;
            registered_waits.push_back(state);
        }
    }
    for (const auto& state : registered_waits) {
        state->cancelled.store(true, std::memory_order_release);
        state->cv.notify_all();
    }
    if (m_sync_manager) m_sync_manager->notify_waiters();

    // Take each port's mutex before notifying. This closes the race where a
    // waiter has checked its predicate but has not yet parked on the CV.
    std::lock_guard<std::mutex> registry_lock(m_iocp_registry_mutex);
    for (auto& [handle, port] : m_iocp_ports) {
        (void)handle;
        std::lock_guard<std::mutex> port_lock(port->mutex);
        port->closed = true;
        port->cv.notify_all();
    }
}

std::vector<std::string> Kernel32ExtManager::inherited_guest_environment()
    const {
    std::vector<std::string> environment;
    if (m_environment_manager == nullptr) return environment;
    for (const auto& [name, value] :
         m_environment_manager->get_all_variables_as_named()) {
        environment.push_back(name + "=" + value);
    }
    return environment;
}

void Kernel32ExtManager::state_primary_thread_suspension(
    std::vector<std::string>& extra_env) {
    if (std::none_of(extra_env.begin(), extra_env.end(), [](const auto& entry) {
            return entry.starts_with("VITREO_PRIMARY_THREAD_SUSPENDED=");
        })) {
        extra_env.emplace_back("VITREO_PRIMARY_THREAD_SUSPENDED=0");
    }
}

int Kernel32ExtManager::launch_guest_worker(
    const std::string& host_image_path,
    const std::string& guest_image_path,
    const std::string& guest_command_line,
    const std::string& guest_current_directory,
    const std::string& inherited_pipes,
    const std::string& inherited_sync,
    const std::string& inherited_mappings,
    std::string_view elevation_credential,
    std::vector<std::string> extra_env,
    std::vector<std::string> guest_environment,
    bool detached,
    const std::string& pseudo_console_handles) {
    if (m_child_process_restricted) {
        VITREO_TRACE(
            "[process] launch rejected by inherited child-process policy\n");
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return -1;
    }
    if (m_process_session == nullptr) {
        VITREO_TRACE("[process] launch rejected: no ProcessSession owner\n");
        return -1;
    }

    const std::string absolute_image =
        capsule::make_absolute_win_path(guest_image_path, m_current_directory);
    if (absolute_image.empty()) {
        VITREO_TRACE("[process] launch rejected: invalid guest path '%s'\n",
                     guest_image_path.c_str());
        return -1;
    }

    ipc::WorkerLaunchSpec spec;
    spec.host_image_path = host_image_path;
    spec.guest_image_path = absolute_image;
    spec.guest_command_line = guest_command_line;
    spec.guest_current_directory =
        guest_current_directory.empty() ? m_current_directory
                                        : guest_current_directory;
    spec.capsule_name = m_capsule_name.empty() ? "default" : m_capsule_name;
    spec.inherited_pipes = inherited_pipes;
    spec.pseudo_console_handles = pseudo_console_handles;
    spec.inherited_sync = inherited_sync;
    spec.inherited_mappings = inherited_mappings;
    spec.elevation_credential = elevation_credential;
    if (elevation_credential.empty() && m_elevation != nullptr) {
        if (m_elevation->origin() ==
            GuestTokenOrigin::LocalSystemService) {
            spec.token_context =
                ipc::WorkerTokenContext::LocalSystemService;
        } else if (m_elevation->origin() ==
                   GuestTokenOrigin::RestrictedService) {
            spec.token_context =
                ipc::WorkerTokenContext::RestrictedService;
        }
    }
    // Shell/legacy launches also need a fresh primary ID: inheriting the
    // caller's runtime environment must not duplicate its thread identity.
    if (std::none_of(extra_env.begin(), extra_env.end(), [](const auto& entry) {
            return entry.starts_with("VITREO_PRIMARY_THREAD_ID=");
        })) {
        const u32 tid = m_thread_manager ? m_thread_manager->allocate_thread_id()
                                       : ipc::allocate_system_thread_id();
        if (tid == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return -1;
        }
        extra_env.emplace_back("VITREO_PRIMARY_THREAD_ID=" + std::to_string(tid));
    }
    state_primary_thread_suspension(extra_env);
    // The foreground start grant (CocoaBridge::kForegroundGrantEnv) is set
    // explicitly either way so a child never inherits its parent's value.
    const bool foreground_grant =
        m_foreground_may_grant && m_foreground_may_grant();
    extra_env.emplace_back(foreground_grant ? "VITREO_FOREGROUND_GRANT=1"
                                            : "VITREO_FOREGROUND_GRANT=0");
    spec.extra_env = std::move(extra_env);
    spec.guest_environment = std::move(guest_environment);
    spec.detached = detached;
    const auto metadata = guest_process_metadata(host_image_path);
    if (m_pre_process_launch) {
        // Each Windows process currently owns an in-memory Registry instance.
        // Persist the parent's completed writes before the child loads its
        // instance from the capsule, matching pre-launch visibility.
        m_pre_process_launch();
    }
    const pid_t child = m_process_session->launch_worker(spec);
    if (child > 0 && foreground_grant && m_foreground_granted) {
        m_foreground_granted(static_cast<int>(child));
    }
    if (child > 0 && metadata.has_value()) {
        if (auto* process = ipc::track_process(child)) {
            process->publish_guest_metadata(*metadata);
            ipc::ipc_log(
                "published guest process metadata pid=%d peb=0x%llx "
                "image_base=0x%llx bitness=%u",
                static_cast<int>(child),
                static_cast<unsigned long long>(metadata->peb_address),
                static_cast<unsigned long long>(metadata->image_base),
                metadata->is_64bit ? 64u : 32u);
        }
    }
    return static_cast<int>(child);
}

void Kernel32ExtManager::set_current_directory(const std::string& dir) {
    // Windows reports the current directory WITHOUT a trailing separator --
    // except at a drive root, which is "C:\\" and never a bare "C:". A bare
    // "C:" is a different thing entirely on Windows (the per-drive current
    // directory), so GetCurrentDirectory/_getcwd handing one back gives every
    // caller that appends a name a path missing its root separator.
    std::string normalised = dir;
    while (normalised.size() > 3 &&
           (normalised.back() == '\\' || normalised.back() == '/')) {
        normalised.pop_back();
    }
    if (normalised.size() == 2 && normalised[1] == ':') {
        normalised += '\\';
    }
    m_current_directory = normalised;
    const std::string& dir_normalised = m_current_directory;
    m_vfs.set_cwd(dir_normalised);
    // Learn (SetCurrentDirectory, Note): "The current directory for a process
    // is locked while the process is executing. That prevents the directory
    // from being deleted, moved, or renamed." Windows holds a traverse handle
    // sharing read|write only (files_ns_probe RemoveDirectoryW_current_dir ->
    // ERROR_SHARING_VIOLATION).
    if (m_cwd_handle != capsule::INVALID_FILE_HANDLE) {
        (void)m_vfs.close(m_cwd_handle);
        m_cwd_handle = capsule::INVALID_FILE_HANDLE;
    }
    auto cwd_open = m_vfs.open(dir_normalised, capsule::AccessMode::Execute,
                               capsule::ShareMode::ReadWrite,
                               capsule::CreationDisposition::OpenExisting);
    if (cwd_open.is_ok()) m_cwd_handle = cwd_open.value();
    if (m_module_manager != nullptr) {
        m_module_manager->set_current_directory(dir_normalised);
    }
    VITREO_TRACE( "[k32ext] set_current_directory: '%s'\n",
                 dir_normalised.c_str());
}

void Kernel32ExtManager::set_module_manager(ModuleManager* mgr) {
    m_module_manager = mgr;
    if (m_module_manager != nullptr) {
        m_module_manager->set_current_directory(m_current_directory);
    }
}

std::optional<std::string> Kernel32ExtManager::process_image_path(
    Handle process_handle) const {
    auto current_image_path = [this]() -> std::optional<std::string> {
        if (m_module_manager == nullptr) return std::nullopt;
        char path[512]{};
        const u32 length = m_module_manager->get_module_filename(
            0, path, static_cast<u32>(sizeof(path)));
        if (length == 0 || length >= sizeof(path)) return std::nullopt;
        return std::string(path, length);
    };

    if (process_handle == CURRENT_PROCESS_PSEUDO) {
        return current_image_path();
    }
    if (m_handle_table == nullptr ||
        m_handle_table->get_type(process_handle) != HandleType::Process) {
        return std::nullopt;
    }

    auto* process = static_cast<ipc::ProcessObject*>(
        m_handle_table->get(process_handle, HandleType::Process));
    if (process == nullptr) return std::nullopt;
    if (m_process_manager != nullptr &&
        static_cast<u32>(process->pid) ==
            m_process_manager->get_current_process_id()) {
        return current_image_path();
    }
    if (!m_process_snapshot_provider) return std::nullopt;

    const auto processes = m_process_snapshot_provider();
    auto it = std::find_if(processes.begin(), processes.end(),
                           [process](const auto& entry) {
                               return entry.pid == static_cast<u32>(process->pid);
                           });
    if (it == processes.end() || it->image_path.empty()) return std::nullopt;
    return it->image_path;
}

// =============================================================================
// Export Table Factory
// =============================================================================

namespace {
// HRESULT APIs preserve last-error even if their allocation helper fails.
struct DescriptionLastError {
    u32 saved{get_last_error()};
    ~DescriptionLastError() { set_last_error(saved); }
};

bool description_handle_width(Marshaler& m) {
    const u64 raw = m.arg(0);
    if (!m.cpu_is_64bit() || raw == ~u64{1} || raw == ~u64{0}) return true;
    if (raw <= UINT32_MAX && raw != static_cast<u32>(-2) &&
        raw != static_cast<u32>(-1)) return true;
    m.set_return(thread_description_contract::NT_INVALID_HANDLE_HR);
    return false;
}

void description_pointer_fault(Marshaler& m, GuestAddr address, usize size,
                               bool write) {
    auto* cpu = m.cpu();
    if (!cpu) return;
    for (usize offset = 0; offset < size; ++offset) {
        if (offset > std::numeric_limits<GuestAddr>::max() - address) break;
        const GuestAddr candidate = address + offset;
        const bool accessible = write
            ? m.memory().is_range_writable(candidate, 1, m.cpu_is_64bit())
            : m.memory().read_u8(candidate).is_ok();
        if (!accessible) { address = candidate; break; }
    }
    auto& pending = cpu->pending_raise;
    pending.clear();
    pending.active = true;
    pending.code = thread_description_contract::ACCESS_VIOLATION;
    pending.num_args = 2;
    pending.args[0] = write ? 1 : 0;
    pending.args[1] = address;
}
} // namespace

std::optional<u32> Kernel32ExtManager::external_thread_description_status(
    Handle handle, u32 required_access) {
    using namespace thread_description_contract;
    std::lock_guard lock(m_external_thread_mutex);
    if (!m_external_primary_handles.contains(handle) || !m_handle_table ||
        m_handle_table->get_type(handle) != HandleType::Thread) return std::nullopt;
    u32 granted = m_handle_table->get_access(handle);
    if (granted & THREAD_QUERY_INFORMATION) granted |= THREAD_QUERY_LIMITED_INFORMATION;
    if (granted & THREAD_SET_INFORMATION) granted |= THREAD_SET_LIMITED_INFORMATION;
    if (!(granted & required_access)) return NT_ACCESS_DENIED_HR;
    static std::atomic<bool> reported_get{false}, reported_set{false};
    const bool set = required_access == THREAD_SET_LIMITED_INFORMATION;
    if (!(set ? reported_set : reported_get).exchange(true))
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!%sThreadDescription: cross-process thread description storage\n", set ? "Set" : "Get");
    return NT_NOT_IMPLEMENTED_HR;
}

void Kernel32ExtManager::impl_SetThreadDescription(Marshaler& m) {
    using namespace thread_description_contract;
    DescriptionLastError last_error;
    const GuestAddr source = m.arg_ptr(1);
    std::u16string description;
    bool too_long = false;
    // Native Windows scans the string before validating the handle. NULL
    // represents an empty description. Failed reads raise on either ABI.
    if (source != 0) {
        GuestAddr cursor = source;
        for (;;) {
            auto ch = m.memory().read_u16(cursor);
            if (ch.is_err()) {
                description_pointer_fault(m, cursor, sizeof(u16), false);
                return;
            }
            if (ch.value() == 0) break;
            if (description.size() == MAX_DESCRIPTION_CHARS) too_long = true;
            else description.push_back(static_cast<char16_t>(ch.value()));
            cursor += sizeof(u16);
            if (!m.cpu_is_64bit()) cursor = static_cast<u32>(cursor);
        }
    }
    if (too_long) {
        m.set_return(NT_NAME_TOO_LONG_HR);
        return;
    }
    if (!description_handle_width(m)) return;
    const Handle thread = static_cast<Handle>(m.arg(0));
    if (auto external = external_thread_description_status(thread, THREAD_SET_LIMITED_INFORMATION)) {
        m.set_return(*external);
        return;
    }
    u32 result = NT_INVALID_HANDLE_HR;
    if (m_thread_manager)
        (void)m_thread_manager->set_thread_description(thread, std::move(description), &result);
    m.set_return(result);
}

void Kernel32ExtManager::impl_GetThreadDescription(Marshaler& m) {
    using namespace thread_description_contract;
    DescriptionLastError last_error;
    const GuestAddr out_pp = m.arg_ptr(1);
    auto write_output = [&](GuestAddr value) {
        return m.cpu_is_64bit() ? m.memory().write_u64(out_pp, value).is_ok()
                               : m.memory().write_u32(out_pp, static_cast<u32>(value)).is_ok();
    };
    // Clear the required output first, even for invalid handles. The x64
    // pointer write is atomic on a boundary fault; x86 writes one DWORD.
    if (!write_output(0)) {
        description_pointer_fault(m, out_pp, m.cpu_is_64bit() ? sizeof(u64) : sizeof(u32), true);
        return;
    }
    if (!description_handle_width(m)) return;
    const Handle thread = static_cast<Handle>(m.arg(0));
    if (auto external = external_thread_description_status(thread, THREAD_QUERY_LIMITED_INFORMATION)) {
        m.set_return(*external);
        return;
    }
    std::u16string description;
    u32 result = NT_INVALID_HANDLE_HR;
    if (!m_thread_manager || !m_thread_manager->get_thread_description(thread, description, &result)) {
        m.set_return(result);
        return;
    }
    if (!m_global_local_manager) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!GetThreadDescription: LocalAlloc owner unavailable\n");
        m.set_return(NT_NOT_IMPLEMENTED_HR);
        return;
    }
    u64 byte_size = (description.size() + 1) * sizeof(char16_t);
    GuestAddr buffer = m_global_local_manager->global_alloc(LPTR, byte_size);
    if (buffer == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (!description.empty()) {
        auto write = m.memory().write_bytes(
            buffer, reinterpret_cast<const Byte*>(description.data()),
            description.size() * sizeof(char16_t));
        if (write.is_err()) {
            (void)m_global_local_manager->global_free(buffer);
            m.set_return(E_OUTOFMEMORY);
            return;
        }
    }
    (void)m.memory().write_u16(buffer + description.size() * sizeof(char16_t), 0);
    if (!write_output(buffer)) {
        (void)m_global_local_manager->global_free(buffer);
        description_pointer_fault(m, out_pp, m.cpu_is_64bit() ? sizeof(u64) : sizeof(u32), true);
        return;
    }
    m.set_return(NT_SUCCESS_HR);
}

} // namespace vitreo::winapi
