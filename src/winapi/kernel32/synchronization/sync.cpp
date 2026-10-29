/// @file sync.cpp
/// @brief Implementation of synchronization primitives (THREAD-002)

#include "sync.hpp"
#include "sync_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/common/timer_resolution.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/sync/ntsync.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "ipc/named_sync.hpp"

#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace vitreo::winapi {

namespace {
// Serialize Interlocked* API emulation so read-modify-write operations are
// atomic across concurrently running guest threads.
std::mutex g_interlocked_api_mutex;
} // namespace



// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void impl_InitializeCriticalSection(Marshaler& m, SyncManager& mgr) {
    GuestAddr cs_addr = m.arg_ptr(0);
    mgr.initialize_critical_section(cs_addr);
}

void impl_InitializeCriticalSectionAndSpinCount(Marshaler& m, SyncManager& mgr) {
    GuestAddr cs_addr = m.arg_ptr(0);
    u32 spin_count    = static_cast<u32>(m.arg(1));
    mgr.initialize_critical_section_and_spin_count(cs_addr, spin_count);
    m.set_return(1);
}

void impl_EnterCriticalSection(Marshaler& m, SyncManager& mgr) {
    GuestAddr cs_addr = m.arg_ptr(0);
    mgr.enter_critical_section(cs_addr);
}

void impl_TryEnterCriticalSection(Marshaler& m, SyncManager& mgr) {
    GuestAddr cs_addr = m.arg_ptr(0);
    bool ok = mgr.try_enter_critical_section(cs_addr);
    m.set_return(ok ? 1u : 0u);
}

void impl_LeaveCriticalSection(Marshaler& m, SyncManager& mgr) {
    GuestAddr cs_addr = m.arg_ptr(0);
    mgr.leave_critical_section(cs_addr);
}

void impl_DeleteCriticalSection(Marshaler& m, SyncManager& mgr) {
    GuestAddr cs_addr = m.arg_ptr(0);
    mgr.delete_critical_section(cs_addr);
}

void impl_OpenMutexW(Marshaler& m, SyncManager& mgr) {
    // HANDLE OpenMutexW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName)
    GuestAddr name_ptr = m.arg_ptr(2);
    std::string name;
    if (name_ptr != 0) {
        auto name_result = m.read_wide_string(name_ptr);
        if (name_result.is_ok()) {
            name = name_result.value();
        }
    }
    Handle h = mgr.open_mutex(name, m.arg32(0), m.arg32(1) != 0);
    VITREO_TRACE("[sync] OpenMutexW(name='%s') => 0x%x\n", name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

void impl_OpenMutexA(Marshaler& m, SyncManager& mgr) {
    // HANDLE OpenMutexA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName)
    GuestAddr name_ptr = m.arg_ptr(2);
    std::string name;
    if (name_ptr != 0) {
        auto name_result = m.read_ansi_string(name_ptr);
        if (name_result.is_ok()) {
            name = name_result.value();
        }
    }
    Handle h = mgr.open_mutex(name, m.arg32(0), m.arg32(1) != 0);
    VITREO_TRACE("[sync] OpenMutexA(name='%s') => 0x%x\n", name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

void impl_OpenSemaphoreW(Marshaler& m, SyncManager& mgr) {
    // HANDLE OpenSemaphoreW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName)
    GuestAddr name_ptr = m.arg_ptr(2);
    std::string name;
    if (name_ptr != 0) {
        auto name_result = m.read_wide_string(name_ptr);
        if (name_result.is_ok()) {
            name = name_result.value();
        }
    }
    Handle h = mgr.open_semaphore(name, m.arg32(0), m.arg32(1) != 0);
    VITREO_TRACE("[sync] OpenSemaphoreW(name='%s') => 0x%x\n", name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

void impl_OpenSemaphoreA(Marshaler& m, SyncManager& mgr) {
    // HANDLE OpenSemaphoreA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName)
    GuestAddr name_ptr = m.arg_ptr(2);
    std::string name;
    if (name_ptr != 0) {
        auto name_result = m.read_ansi_string(name_ptr);
        if (name_result.is_ok()) {
            name = name_result.value();
        }
    }
    Handle h = mgr.open_semaphore(name, m.arg32(0), m.arg32(1) != 0);
    VITREO_TRACE("[sync] OpenSemaphoreA(name='%s') => 0x%x\n", name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

namespace {
bool capture_sync_security(Marshaler& m,GuestAddr attributes,object_access::Descriptor& out) {
    if(!attributes)return true;
    const GuestAddr descriptor_slot=attributes+(m.cpu_is_64bit()?8:4);
    const auto pointer=m.cpu_is_64bit()?m.read_u64(descriptor_slot).value_or(0):m.read_u32(descriptor_slot).value_or(0);
    if(!pointer)return true;
    const auto revision=m.read_u8(pointer);
    if(revision.is_err()) {set_last_error(error_codes::ERROR_NOACCESS);return false;}
    if(revision.value()!=1) {set_last_error(sync_access::ERROR_UNKNOWN_REVISION);return false;} // Windows sync_security_probe: ERROR_UNKNOWN_REVISION.
    const auto control=m.read_u16(pointer+2);
    if(control.is_err()){set_last_error(error_codes::ERROR_NOACCESS);return false;}
    const bool relative=(control.value()&sync_access::SE_SELF_RELATIVE)!=0;
    std::array<Byte,40> header{};
    const usize header_size=relative?20u:(m.cpu_is_64bit()?40u:20u);
    if(m.memory().read_bytes(pointer,header.data(),header_size).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);return false;
    }
    auto component=[&](u32 index)->GuestAddr {
        if(relative){const auto offset=m.read_u32(pointer+4+4*index).value_or(0);return offset?pointer+offset:0;}
        const auto slot=pointer+(m.cpu_is_64bit()?8+8*index:4+4*index);
        return m.cpu_is_64bit()?m.read_u64(slot).value_or(0):m.read_u32(slot).value_or(0);
    };
    if(const GuestAddr owner=component(0);owner){
        const auto count=m.read_u8(owner+1);
        if(count.is_err() || count.value()>15){set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);return false;}
        out.owner.resize(8+4u*count.value());
        if(m.memory().read_bytes(owner,reinterpret_cast<Byte*>(out.owner.data()),out.owner.size()).is_err() ||
            !object_access::valid_sid(out.owner)){set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);return false;}
    }
    if(control.value()&sync_access::SE_SACL_PRESENT){
        if(const GuestAddr sacl=component(2);sacl){
            // Measured (tools/oracle_probe/sync_sacl_label_probe.c): a SACL
            // holding a mandatory label and/or audit ACEs is accepted; audit
            // ACEs have no effect on access. Only the label is kept.
            const auto size=m.read_u16(sacl+2);
            const auto count=m.read_u16(sacl+4);
            if(size.is_err() || count.is_err() || size.value()<8){
                set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);return false;
            }
            constexpr u8 SYSTEM_AUDIT_ACE_TYPE=0x2, SYSTEM_ALARM_ACE_TYPE=0x3,
                         SYSTEM_MANDATORY_LABEL_ACE_TYPE=0x11;
            u32 offset=8;
            for(u32 i=0;i<count.value();++i){
                const auto type=m.read_u8(sacl+offset);
                const auto flags=m.read_u8(sacl+offset+1);
                const auto ace_size=m.read_u16(sacl+offset+2);
                if(type.is_err()||flags.is_err()||ace_size.is_err()||ace_size.value()<8||
                    offset+ace_size.value()>size.value()){
                    set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);return false;
                }
                if(!(flags.value()&sync_access::INHERIT_ONLY_ACE)){
                    if(type.value()==SYSTEM_MANDATORY_LABEL_ACE_TYPE){
                        // Mask, then the label SID S-1-16-RID.
                        const auto policy=m.read_u32(sacl+offset+4);
                        const auto sub_count=m.read_u8(sacl+offset+9);
                        if(policy.is_err()||sub_count.is_err()||sub_count.value()==0||
                            16u+4u*sub_count.value()>ace_size.value()){
                            set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);return false;
                        }
                        const auto rid=m.read_u32(sacl+offset+8+8+4u*(sub_count.value()-1u));
                        if(rid.is_err()){set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);return false;}
                        out.has_label=true;out.label_rid=rid.value();out.label_policy=policy.value();
                    } else if(type.value()==SYSTEM_AUDIT_ACE_TYPE || type.value()==SYSTEM_ALARM_ACE_TYPE){
                        out.has_audit=true;
                    } else {
                        out.unsupported_sacl=true;
                    }
                }
                offset+=ace_size.value();
            }
        }
    }
    if(control.value()&sync_access::SE_DACL_PRESENT){
        const GuestAddr dacl=component(3);
        if(!dacl)out.dacl.reset();
        else {
            const auto size=m.read_u16(dacl+2);
            if(size.is_err() || size.value()<8){set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);return false;}
            out.dacl.emplace(size.value());
            if(m.memory().read_bytes(dacl,reinterpret_cast<Byte*>(out.dacl->data()),out.dacl->size()).is_err()){
                set_last_error(error_codes::ERROR_NOACCESS);return false;
            }
        }
    }
    return true;
}
SyncManager::SecurityFactory sync_security_factory(Marshaler& m,GuestAddr attributes) {
    return [&m,attributes](object_access::Descriptor& descriptor){return capture_sync_security(m,attributes,descriptor);};
}
void impl_SyncEx(Marshaler& m,SyncManager& mgr,bool mutex,bool wide) {
    const auto attributes=m.arg_ptr(0);const auto name_ptr=m.arg_ptr(1);
    const u32 flags=m.arg32(2);const u32 access=m.arg32(3);
    const u32 valid_flags=mutex?sync_access::CREATE_MUTEX_INITIAL_OWNER:
        sync_access::CREATE_EVENT_MANUAL_RESET|sync_access::CREATE_EVENT_INITIAL_SET;
    if(flags&~valid_flags){set_last_error(error_codes::ERROR_INVALID_PARAMETER);m.set_return(0);return;}
    std::string name;
    if(name_ptr){
        const auto value=wide?m.read_wide_string(name_ptr):m.read_ansi_string(name_ptr);
        if(value.is_err()){set_last_error(error_codes::ERROR_NOACCESS);m.set_return(0);return;}
        name=value.value();
    }
    bool inherit=false;
    if(attributes){
        const auto value=m.read_u32(attributes+(m.cpu_is_64bit()?16:8));
        if(value.is_err()){set_last_error(error_codes::ERROR_NOACCESS);m.set_return(0);return;}
        inherit=value.value()!=0;
    }
    const auto factory=sync_security_factory(m,attributes);
    const Handle h=mutex?mgr.create_mutex((flags&sync_access::CREATE_MUTEX_INITIAL_OWNER)!=0,name,inherit,access,factory):
        mgr.create_event((flags&sync_access::CREATE_EVENT_MANUAL_RESET)!=0,
            (flags&sync_access::CREATE_EVENT_INITIAL_SET)!=0,name,inherit,
            make_sync_trace_context(m,wide?"CreateEventExW":"CreateEventExA",mgr.causal_trace_enabled()),access,factory);
    m.set_return(h);
}
}

void impl_CreateMutexW(Marshaler& m, SyncManager& mgr) {
    bool initial_owner = m.arg(1) != 0;
    GuestAddr name_ptr = m.arg_ptr(2);
    std::string name;
    if (name_ptr != 0) {
        auto name_result = m.read_wide_string(name_ptr);
        if (name_result.is_ok()) {
            name = name_result.value();
        }
    }
    const auto attributes=m.arg_ptr(0);
    const bool inherit=attributes && m.read_u32(attributes+(m.cpu_is_64bit()?16:8)).value_or(0)!=0;
    Handle h = mgr.create_mutex(initial_owner,name,inherit,sync_access::MUTEX_ALL_ACCESS,sync_security_factory(m,attributes));
    m.set_return(static_cast<u64>(h));
}

void impl_CreateMutexA(Marshaler& m, SyncManager& mgr) {
    bool initial_owner = m.arg(1) != 0;
    GuestAddr name_ptr = m.arg_ptr(2);
    std::string name;
    if (name_ptr != 0) {
        auto name_result = m.read_ansi_string(name_ptr);
        if (name_result.is_ok()) {
            name = name_result.value();
        }
    }
    const auto attributes=m.arg_ptr(0);
    const bool inherit=attributes && m.read_u32(attributes+(m.cpu_is_64bit()?16:8)).value_or(0)!=0;
    Handle h = mgr.create_mutex(initial_owner,name,inherit,sync_access::MUTEX_ALL_ACCESS,sync_security_factory(m,attributes));
    VITREO_TRACE("[sync] CreateMutexA(initial_owner=%d, name='%s') -> 0x%x\n",
                 initial_owner, name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

void impl_ReleaseMutex(Marshaler& m, SyncManager& mgr) {
    Handle handle = static_cast<Handle>(m.arg(0));
    bool ok = mgr.release_mutex(handle);
    m.set_return(ok ? 1u : 0u);
}

void impl_CreateEventW(Marshaler& m, SyncManager& mgr) {
    const GuestAddr security_attributes = m.arg_ptr(0);
    bool manual_reset  = m.arg(1) != 0;
    bool initial_state = m.arg(2) != 0;
    GuestAddr name_ptr = m.arg_ptr(3);
    std::string name;
    if (name_ptr != 0) {
        auto name_result = m.read_wide_string(name_ptr);
        if (name_result.is_ok()) {
            name = name_result.value();
        }
    }
    bool inheritable = false;
    if (security_attributes != 0) {
        const GuestAddr offset = m.cpu_is_64bit() ? 16 : 8;
        auto value = m.read_u32(security_attributes + offset);
        inheritable = value.is_ok() && value.value() != 0;
    }
    const SyncTraceContext trace = make_sync_trace_context(
        m, "CreateEventW", mgr.causal_trace_enabled());
    Handle h = mgr.create_event(
        manual_reset, initial_state, name, inheritable, trace,
        sync_access::EVENT_ALL_ACCESS,sync_security_factory(m,security_attributes));
    if (mgr.causal_trace_enabled()) {
        ipc::ipc_log(
            "CreateEventW h=0x%x manual=%d init=%d name='%s' "
            "caller=0x%llx parents=%s",
            static_cast<unsigned>(h), manual_reset ? 1 : 0,
            initial_state ? 1 : 0, name.c_str(),
            static_cast<unsigned long long>(trace.caller),
            format_parent_chain(trace.parents, trace.parent_count).c_str());
    }
    VITREO_TRACE("[sync] CreateEventW(manual=%d, init=%d, name=\"%s\") -> 0x%x\n",
                manual_reset, initial_state, name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

void impl_CreateEventA(Marshaler& m, SyncManager& mgr) {
    // HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL bManualReset,
    //                     BOOL bInitialState, LPCSTR lpName)
    const GuestAddr security_attributes = m.arg_ptr(0);
    bool manual_reset  = m.arg(1) != 0;
    bool initial_state = m.arg(2) != 0;
    GuestAddr name_ptr = m.arg_ptr(3);
    std::string name;
    if (name_ptr != 0) {
        auto name_result = m.read_ansi_string(name_ptr, 260);
        if (name_result.is_ok()) {
            name = name_result.value();
        }
    }
    bool inheritable = false;
    if (security_attributes != 0) {
        const GuestAddr offset = m.cpu_is_64bit() ? 16 : 8;
        auto value = m.read_u32(security_attributes + offset);
        inheritable = value.is_ok() && value.value() != 0;
    }
    Handle h = mgr.create_event(
        manual_reset, initial_state, name, inheritable,
        make_sync_trace_context(
            m, "CreateEventA", mgr.causal_trace_enabled()),
        sync_access::EVENT_ALL_ACCESS,sync_security_factory(m,security_attributes));
    VITREO_TRACE( "[sync] CreateEventA(manual=%d, init=%d, name=\"%s\") -> 0x%x\n",
                manual_reset, initial_state, name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

void impl_CreateEventExW(Marshaler& m, SyncManager& mgr) {
    impl_SyncEx(m,mgr,false,true);
}

void impl_OpenEventW(Marshaler& m, SyncManager& mgr) {
    const u32 desired_access = static_cast<u32>(m.arg(0));
    const bool inheritable = m.arg(1) != 0;
    const GuestAddr name_ptr = m.arg_ptr(2);
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::string name;
    if (name_ptr != 0) {
        auto result = m.read_wide_string(name_ptr);
        if (result.is_ok()) name = result.value();
    }
    const Handle h = mgr.open_event(desired_access, name, inheritable);
    VITREO_TRACE("[sync] OpenEventW(access=0x%x, inherit=%d, name=\"%s\") -> 0x%x\n",
                 desired_access, inheritable, name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

void impl_OpenEventA(Marshaler& m, SyncManager& mgr) {
    const u32 desired_access = static_cast<u32>(m.arg(0));
    const bool inheritable = m.arg(1) != 0;
    const GuestAddr name_ptr = m.arg_ptr(2);
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::string name;
    if (name_ptr != 0) {
        auto result = m.read_ansi_string(name_ptr, 260);
        if (result.is_ok()) name = result.value();
    }
    const Handle h = mgr.open_event(desired_access, name, inheritable);
    VITREO_TRACE("[sync] OpenEventA(access=0x%x, inherit=%d, name=\"%s\") -> 0x%x\n",
                 desired_access, inheritable, name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

SyncManager::SecurityFactory sync_security_attributes_factory(Marshaler& m, GuestAddr attributes) {
    return sync_security_factory(m, attributes);
}

void impl_SetEvent(Marshaler& m, SyncManager& mgr) {
    // BOOL SetEvent(HANDLE hEvent) — Learn: nonzero on success; on failure
    // zero and GetLastError (typically ERROR_INVALID_HANDLE).
    Handle handle = static_cast<Handle>(m.arg(0));
    bool ok = mgr.set_event(
        handle, make_sync_trace_context(
                    m, "SetEvent", mgr.causal_trace_enabled()));
    if (!ok) {
        ipc::ipc_log("SetEvent FAIL h=0x%x gle=%u",
                     static_cast<unsigned>(handle),
                     error_codes::ERROR_INVALID_HANDLE);
    }
    // SetEvent never blocks the caller. Waiters are separate host threads
    // woken by the signal itself; a timed sleep here is coalesced by macOS to
    // tens of milliseconds and stalls whatever lock the caller holds.
    VITREO_TRACE("[sync] SetEvent(0x%x) -> %s\n", handle, ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1u : 0u);
}

void impl_ResetEvent(Marshaler& m, SyncManager& mgr) {
    // BOOL ResetEvent(HANDLE hEvent) — Learn: nonzero on success; on failure
    // zero and GetLastError (ERROR_INVALID_HANDLE / ERROR_ACCESS_DENIED).
    Handle handle = static_cast<Handle>(m.arg(0));
    bool ok = mgr.reset_event(
        handle, make_sync_trace_context(
                    m, "ResetEvent", mgr.causal_trace_enabled()));
    if (!ok && get_last_error() == 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
    }
    m.set_return(ok ? 1u : 0u);
}

void impl_PulseEvent(Marshaler& m, SyncManager& mgr) {
    // BOOL PulseEvent(HANDLE hEvent) — Learn: signals the event, releases the
    // threads already waiting on it, then resets it to non-signalled in one
    // step. A thread that was not yet waiting misses the pulse, which is why
    // Microsoft documents the call as unreliable; it is still what a caller
    // gets, so set-then-reset reproduces both the release and the miss.
    //
    // Implementing it matters beyond the semantics: the generic missing-export
    // stub pops 0 stdcall bytes, so an unimplemented PulseEvent left this
    // call's 4 argument bytes on the guest stack and the caller returned to a
    // garbage address.
    Handle handle = static_cast<Handle>(m.arg(0));
    bool ok = mgr.pulse_event(
        handle, make_sync_trace_context(
                    m, "PulseEvent", mgr.causal_trace_enabled()));
    VITREO_TRACE("[sync] PulseEvent(0x%x) -> %s\n", handle,
                 ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1u : 0u);
}

void impl_WaitForSingleObject(Marshaler& m, SyncManager& mgr) {
    Handle handle    = static_cast<Handle>(m.arg(0));
    u32 milliseconds = static_cast<u32>(m.arg(1));
    // Pair VITREO_THREAD_DIAG's CreateThread records with the first blocking
    // wait reached by each guest thread. This is intentionally bounded and
    // opt-in: it makes a stuck worker's guest caller address and handle
    // observable without enabling the prohibitively noisy full instruction
    // trace. Include the handle in the de-duplication key because one caller
    // waits on many distinct events.
    static const bool thread_diag_enabled = [] {
        const char* value = std::getenv("VITREO_THREAD_DIAG");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    if (thread_diag_enabled && milliseconds == INFINITE_WAIT && m.has_cpu()) {
            struct WaitDiagKey {
                u32 tid;
                Handle handle;
                u64 caller;

                bool operator==(const WaitDiagKey&) const = default;
            };
            static std::mutex s_wait_diag_mu;
            static std::vector<WaitDiagKey> s_wait_diag_seen;
            auto* cpu = m.cpu();
            u64 caller = 0;
            if (cpu->is_64bit()) {
                auto read = m.memory().read_u64(
                    static_cast<GuestAddr>(cpu->gpr.rsp));
                if (read.is_ok()) caller = read.value();
            } else {
                auto read = m.memory().read_u32(
                    static_cast<GuestAddr>(cpu->sp()));
                if (read.is_ok()) caller = read.value();
            }
            const u32 tid = ThreadManager::get_current_thread_id_static();
            const WaitDiagKey key{tid, handle, caller};
            std::lock_guard<std::mutex> lock(s_wait_diag_mu);
            if (s_wait_diag_seen.size() < 128 &&
                std::find(s_wait_diag_seen.begin(),
                          s_wait_diag_seen.end(),
                          key) == s_wait_diag_seen.end()) {
                s_wait_diag_seen.push_back(key);
                std::fprintf(
                    stderr,
                    "[thread-diag] pid=%d wait self=%u handle=0x%x "
                    "caller_ret=0x%llx rip=0x%llx ms=INFINITE\n",
                    static_cast<int>(::getpid()), tid,
                    static_cast<unsigned>(handle),
                    static_cast<unsigned long long>(caller),
                    static_cast<unsigned long long>(cpu->rip));
            }
    }
    const SyncTraceContext trace = make_sync_trace_context(
        m, "WaitForSingleObject", mgr.causal_trace_enabled());
    // The synchronization backend measures only time actually parked. This
    // keeps always-on phase accounting without clocking immediately satisfied
    // waits. Explicit profiling still measures the complete API call.
    const bool profile_wait = g_wait_profile.enabled ||
                              diagnostics::perf_window_active();
    const auto t0 = profile_wait ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    u64 blocked_ns = 0;
    u32 ret = mgr.wait_for_single_object(
        handle, milliseconds, trace, &blocked_ns);
    diagnostics::phase_add_wfso_wall(blocked_ns);
    u64 ns = blocked_ns;
    if (profile_wait) {
        const auto t1 = std::chrono::steady_clock::now();
        ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count());
    }
    if (g_wait_profile.enabled || diagnostics::perf_window_active()) {
        if (g_wait_profile.enabled) {
            auto& s = g_wait_profile.get(static_cast<u32>(handle));
            s.count.fetch_add(1, std::memory_order_relaxed);
            s.total_ns.fetch_add(ns, std::memory_order_relaxed);
            if (milliseconds == 0) s.zero_ms_count.fetch_add(1, std::memory_order_relaxed);
            if (ret == WAIT_TIMEOUT) s.timeout_count.fetch_add(1, std::memory_order_relaxed);
            g_wait_profile.maybe_dump();
        }
        diagnostics::perf_add_wait(
            diagnostics::PerfWaitKind::WaitForSingleObject, ns);
    }
    VITREO_TRACE("[sync] WaitForSingleObject(0x%x, %u) -> 0x%x\n", handle, milliseconds, ret);
    m.set_return(static_cast<u64>(ret));
}

void impl_SignalObjectAndWait(Marshaler& m, SyncManager& mgr) {
    // DWORD SignalObjectAndWait(HANDLE hObjectToSignal, HANDLE hObjectToWaitOn,
    //                           DWORD dwMilliseconds, BOOL bAlertable)
    const Handle signal = static_cast<Handle>(m.arg(0));
    const Handle wait = static_cast<Handle>(m.arg(1));
    const u32 milliseconds = static_cast<u32>(m.arg(2));
    const bool alertable = m.arg32(3) != 0;
    const bool time_wait = diagnostics::perf_window_active();
    const auto t0 = time_wait ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
    const u32 ret =
        mgr.signal_object_and_wait(signal, wait, milliseconds, alertable);
    if (time_wait) {
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        diagnostics::perf_add_wait(
            diagnostics::PerfWaitKind::SignalObjectAndWait, ns);
    }
    VITREO_TRACE(
        "[sync] SignalObjectAndWait(signal=0x%x wait=0x%x ms=%u alert=%u) "
        "-> 0x%x\n",
        signal, wait, milliseconds, alertable ? 1u : 0u, ret);
    m.set_return(static_cast<u64>(ret));
}

namespace {

[[nodiscard]] bool read_wait_handle_array(Marshaler& m, u32 count,
                                          GuestAddr handles_ptr,
                                          std::vector<Handle>& handles_vec) {
    // Both a zero count and one past MAXIMUM_WAIT_OBJECTS are
    // ERROR_INVALID_PARAMETER on Windows. WAIT_FAILED without an error code
    // leaves the guest reading whatever the previous call happened to set.
    if (count == 0 || count > 64) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const u32 handle_stride = m.cpu_is_64bit() ? 8u : 4u;
    handles_vec.resize(count);
    for (u32 i = 0; i < count; ++i) {
        const auto handle_addr =
            static_cast<GuestAddr>(handles_ptr) + i * handle_stride;
        const auto h_result = m.cpu_is_64bit()
            ? m.memory().read_u64(handle_addr)
            : m.memory().read_u32(handle_addr).map(
                  [](u32 v) { return static_cast<u64>(v); });
        if (h_result.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
        handles_vec[i] = static_cast<Handle>(h_result.value());
    }
    return true;
}

} // namespace

void impl_WaitForMultipleObjects(Marshaler& m, SyncManager& mgr) {
    const u32 count = static_cast<u32>(m.arg(0));
    const GuestAddr handles_ptr = m.arg_ptr(1);
    const bool wait_all = m.arg(2) != 0;
    const u32 milliseconds = static_cast<u32>(m.arg(3));

    std::vector<Handle> handles_vec;
    if (!read_wait_handle_array(m, count, handles_ptr, handles_vec)) {
        m.set_return(static_cast<u64>(WAIT_FAILED));
        return;
    }

    const bool time_wait = diagnostics::perf_window_active();
    const auto t0 = time_wait ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
    const u32 ret = mgr.wait_for_multiple_objects(
        count, handles_vec.data(), wait_all, milliseconds);
    if (time_wait) {
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        diagnostics::perf_add_wait(
            diagnostics::PerfWaitKind::WaitForMultipleObjects, ns);
    }
    m.set_return(static_cast<u64>(ret));
}

void impl_WaitForMultipleObjectsEx(Marshaler& m, SyncManager& mgr) {
    // DWORD WaitForMultipleObjectsEx(DWORD nCount, const HANDLE* lpHandles,
    //   BOOL bWaitAll, DWORD dwMilliseconds, BOOL bAlertable)
    // Learn: bAlertable TRUE → pending user APCs return WAIT_IO_COMPLETION.
    const u32 count = static_cast<u32>(m.arg(0));
    const GuestAddr handles_ptr = m.arg_ptr(1);
    const bool wait_all = m.arg32(2) != 0;
    const u32 milliseconds = static_cast<u32>(m.arg(3));
    // BOOL in the x64 stack slot [RSP+0x28]: only the low dword is defined.
    const bool alertable = m.arg32(4) != 0;

    std::vector<Handle> handles_vec;
    if (!read_wait_handle_array(m, count, handles_ptr, handles_vec)) {
        m.set_return(static_cast<u64>(WAIT_FAILED));
        return;
    }

    if (alertable && mgr.try_drain_alertable_apcs()) {
        m.set_return(static_cast<u64>(WAIT_IO_COMPLETION));
        return;
    }

    if (!alertable) {
        const bool time_wait = diagnostics::perf_window_active();
        const auto t0 = time_wait ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
        const u32 ret = mgr.wait_for_multiple_objects(
            count, handles_vec.data(), wait_all, milliseconds);
        if (time_wait) {
            const u64 ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
            diagnostics::perf_add_wait(
                diagnostics::PerfWaitKind::WaitForMultipleObjects, ns);
        }
        m.set_return(static_cast<u64>(ret));
        return;
    }

    const bool infinite = milliseconds == INFINITE_WAIT;
    u32 remaining = milliseconds;
    // wait-any: park on the handles and this thread's APC event together, so
    // a queued APC ends the wait at once. wait-all cannot carry an extra
    // object (it would have to be signalled too) and keeps the re-check slice.
    static const bool s_apc_off = sync_mechanism_off("apcevent");
    if (!wait_all && !s_apc_off) {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(infinite ? 0 : milliseconds);
        const u64 apc_ev = mgr.apc_wait_object_for_current_thread();
        for (bool first_probe = true;; first_probe = false) {
            if (mgr.try_drain_alertable_apcs()) {
                m.set_return(static_cast<u64>(WAIT_IO_COMPLETION));
                return;
            }
            u32 left = INFINITE_WAIT;
            if (!infinite) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    // A zero/elapsed timeout still tests the handles once
                    // (Learn; handles_waits_probe wfmoex_any_0_alertable).
                    if (!first_probe) {
                        m.set_return(static_cast<u64>(WAIT_TIMEOUT));
                        return;
                    }
                    left = 0;
                } else {
                    left = static_cast<u32>(std::chrono::duration_cast<
                        std::chrono::milliseconds>(deadline - now).count());
                }
            }
            const u32 ret = mgr.wait_for_multiple_objects(
                count, handles_vec.data(), false, left, apc_ev);
            if (ret == WAIT_OBJECT_0 + count) continue;
            m.set_return(static_cast<u64>(ret));
            return;
        }
    }
    constexpr u32 kSliceMs = 5u;
    for (;;) {
        if (mgr.try_drain_alertable_apcs()) {
            m.set_return(static_cast<u64>(WAIT_IO_COMPLETION));
            return;
        }
        const u32 slice = infinite ? kSliceMs
                                   : (remaining < kSliceMs ? remaining : kSliceMs);
        const u32 ret = mgr.wait_for_multiple_objects(
            count, handles_vec.data(), wait_all, slice);
        if (ret != WAIT_TIMEOUT) {
            m.set_return(static_cast<u64>(ret));
            return;
        }
        if (!infinite) {
            if (remaining <= slice) {
                m.set_return(static_cast<u64>(WAIT_TIMEOUT));
                return;
            }
            remaining -= slice;
        }
    }
}

// Which guest code is doing the sleeping. The load is paced by ~7,300 Sleep(10)
// calls, so the loop that issues them is the loop that decides how long the
// load takes -- and the only way to see it is the return address, since the
// sleep itself carries no identity.
namespace {
struct SleepCallers {
    bool on = std::getenv("VITREO_SLEEP_CALLERS") != nullptr;
    std::mutex mu;
    std::unordered_map<u32, std::pair<u64, u64>> by_ret;  // ret -> {count, ms}
    u64 n = 0;
    void add(u32 ret, u32 ms) {
        std::lock_guard<std::mutex> lk(mu);
        auto& e = by_ret[ret];
        ++e.first;
        e.second += ms;
        if ((++n % 2000) != 0) return;
        std::vector<std::pair<u32, std::pair<u64, u64>>> rows(by_ret.begin(),
                                                              by_ret.end());
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) {
                      return a.second.second > b.second.second;
                  });
        std::fprintf(stderr, "[sleep-callers] after %llu sleeps:\n",
                     (unsigned long long)n);
        for (usize i = 0; i < rows.size() && i < 8; ++i) {
            std::fprintf(stderr,
                "[sleep-callers]   ret=0x%08x n=%-7llu total=%7.2fs avg=%.1fms\n",
                rows[i].first,
                (unsigned long long)rows[i].second.first,
                rows[i].second.second / 1000.0,
                static_cast<double>(rows[i].second.second) /
                    static_cast<double>(rows[i].second.first));
        }
    }
};
SleepCallers g_sleep_callers;
} // namespace

// Same accounting as record_fast_sleep in thunk_fast_path.cpp, for the path
// native32 actually takes. Having it on both sides makes Vitreo-on-Intel and
// Vitreo-on-ARM directly comparable -- one engine, one guest, two numbers --
// using native Windows.
// `concurrent` is the count of threads inside Sleep at once: that, not CPU, is
// what says whether the guest's tick loop is running wide or single-file.
namespace {
struct SleepStats {
    bool on = std::getenv("VITREO_SLEEP_STATS") != nullptr;
    std::mutex mu;
    u64 n = 0, total_ns = 0;
    std::unordered_map<u32, u64> requested;
    std::atomic<int> in_sleep{0};
    std::atomic<int> max_in_sleep{0};
    std::chrono::steady_clock::time_point first{};
    void enter() {
        const int now = in_sleep.fetch_add(1) + 1;
        int prev = max_in_sleep.load();
        while (now > prev && !max_in_sleep.compare_exchange_weak(prev, now)) {}
    }
    void leave(u32 ms, u64 actual_ns) {
        in_sleep.fetch_sub(1);
        std::lock_guard<std::mutex> lk(mu);
        if (n == 0) first = std::chrono::steady_clock::now();
        ++n;
        total_ns += actual_ns;
        ++requested[ms];
        if ((n % 1000) != 0) return;
        const double wall =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - first).count() / 1000.0;
        std::vector<std::pair<u32, u64>> rows(requested.begin(),
                                              requested.end());
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::fprintf(stderr,
            "[sleep-stats] n=%llu total=%.2fs wall=%.2fs concurrent=%.2f "
            "peak=%d  top:",
            (unsigned long long)n, static_cast<double>(total_ns) / 1e9, wall,
            wall > 0 ? static_cast<double>(total_ns) / 1e9 / wall : 0.0,
            max_in_sleep.load());
        for (usize i = 0; i < rows.size() && i < 5; ++i) {
            std::fprintf(stderr, " %ums x%llu", rows[i].first,
                         (unsigned long long)rows[i].second);
        }
        std::fprintf(stderr, "\n");
    }
};
SleepStats g_sleep_stats;
} // namespace

void impl_Sleep(Marshaler& m, SyncManager& mgr) {
    ThreadManager::refresh_current_thread_scheduling();
    u32 milliseconds = static_cast<u32>(m.arg(0));
    const auto stats_t0 = g_sleep_stats.on ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
    if (g_sleep_stats.on) g_sleep_stats.enter();
    struct StatsGuard {
        bool on; u32 ms; std::chrono::steady_clock::time_point t0;
        ~StatsGuard() {
            if (!on) return;
            g_sleep_stats.leave(ms, static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count()));
        }
    } stats_guard{g_sleep_stats.on, milliseconds, stats_t0};
    if (g_sleep_callers.on) {
        g_sleep_callers.add(static_cast<u32>(m.guest_return_address()),
                            milliseconds);
    }
    if (perf::enabled()) {
        // Overshoot, not duration: a Sleep(1) that takes 1 ms is correct and a
        // Sleep(1) that takes 9 ms is a frame-pacing bug. Recording the excess
        // over the request is what makes those two distinguishable in the
        // histogram rather than both landing in the same bucket.
        perf::count(perf::Counter::sleep_calls);
        if (milliseconds == 0) perf::count(perf::Counter::sleep_zero);
        const auto start = perf::now();
        mgr.sleep(milliseconds);
        const u64 actual_ns = perf::since(start);
        const u64 requested_ns = static_cast<u64>(milliseconds) * 1'000'000ULL;
        perf::record(perf::Hist::sleep_overshoot,
                     actual_ns > requested_ns ? actual_ns - requested_ns : 0);
        diagnostics::phase_add_sleep_wall(actual_ns);
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    mgr.sleep(milliseconds);
    auto t1 = std::chrono::steady_clock::now();
    const u64 ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    diagnostics::phase_add_sleep_wall(ns);
    if (g_wait_profile.enabled || diagnostics::perf_window_active()) {
        if (g_wait_profile.enabled) {
            g_wait_profile.sleep_count.fetch_add(1, std::memory_order_relaxed);
            g_wait_profile.sleep_total_ns.fetch_add(ns, std::memory_order_relaxed);
            if (milliseconds == 0) g_wait_profile.sleep_zero_count.fetch_add(1, std::memory_order_relaxed);
        }
        diagnostics::perf_add_wait(diagnostics::PerfWaitKind::Sleep, ns);
    }
}

void impl_SleepEx(Marshaler& m, SyncManager& mgr) {
    ThreadManager::refresh_current_thread_scheduling();
    if (g_sleep_callers.on) {
        g_sleep_callers.add(static_cast<u32>(m.guest_return_address()),
                            static_cast<u32>(m.arg(0)));
    }
    // DWORD SleepEx(DWORD dwMilliseconds, BOOL bAlertable)
    // Learn: returns WAIT_IO_COMPLETION when alertable and APCs/I/O completions
    // run; otherwise 0 when the timeout elapses.
    const u32 milliseconds = static_cast<u32>(m.arg(0));
    const bool alertable = m.arg(1) != 0;
    const auto sleep_t0 = std::chrono::steady_clock::now();
    const auto finish_sleep = [&]() {
        const auto sleep_t1 = std::chrono::steady_clock::now();
        diagnostics::phase_add_sleep_wall(static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(sleep_t1 -
                                                                sleep_t0)
                .count()));
    };

    if (alertable && mgr.try_drain_alertable_apcs()) {
        finish_sleep();
        m.set_return(static_cast<u64>(WAIT_IO_COMPLETION));
        return;
    }

    if (!alertable) {
        mgr.sleep(milliseconds);
        finish_sleep();
        m.set_return(0);
        return;
    }

    // Windows ends an alertable wait as soon as an APC is queued. Wait on the
    // APC notification instead of polling: the slice below is now only an upper
    // bound on how long an *un-notified* wakeup can take, not the granularity
    // at which completions are discovered. Sampling the generation before the
    // first drain closes the race where an APC lands between the check and the
    // wait.
    // alertable_sleep is woken by note_apc_queued under the same mutex it
    // waits on, so nothing is discovered by re-checking: sleep the whole
    // remaining time in one park (bounded to keep the u32 arithmetic honest).
    const bool infinite = milliseconds == INFINITE_WAIT;
    u32 remaining = milliseconds;
    constexpr u32 kSliceMs = 3'600'000u;
    u64 apc_generation = mgr.apc_generation();
    for (;;) {
        if (mgr.try_drain_alertable_apcs()) {
            finish_sleep();
            m.set_return(static_cast<u64>(WAIT_IO_COMPLETION));
            return;
        }
        const u32 slice = infinite ? kSliceMs
                                   : (remaining < kSliceMs ? remaining : kSliceMs);
        if (mgr.alertable_sleep(slice, apc_generation)) {
            continue;   // an APC was queued -- re-check without burning the slice
        }
        if (!infinite) {
            if (remaining <= slice) {
                finish_sleep();
                m.set_return(0);
                return;
            }
            remaining -= slice;
        }
    }
}

// =============================================================================
// Waitable Timers — modeled as events (manual- or auto-reset).
//
// CreateWaitableTimer{W,ExW} returns an event-backed handle.  SetWaitableTimer
// {,Ex} reads the LARGE_INTEGER due time (negative = relative, in 100ns
// units; positive = absolute FILETIME) and schedules a detached host thread
// that sleeps then SetEvent and, when requested, queues PTIMERAPCROUTINE on
// the thread that called SetWaitableTimer. lPeriod > 0 repeats until
// CancelWaitableTimer or a new SetWaitableTimer.
// =============================================================================
namespace {
// Convert a guest LARGE_INTEGER (signed 64-bit, 100-ns units) into a host
// duration. Negative = relative; positive = absolute FILETIME (epoch 1601).
// Returns std::chrono::milliseconds; clamped to >=0.
std::chrono::milliseconds parse_due_time(int64_t due_100ns) {
    if (due_100ns < 0) {
        // Relative — magnitude is 100-ns intervals.
        int64_t ns = -due_100ns * 100;
        if (ns < 0) ns = 0;
        // A timer never signals before its due time; round up.
        return std::chrono::ceil<std::chrono::milliseconds>(
            std::chrono::nanoseconds(ns));
    }
    // Absolute FILETIME: 100-ns ticks since 1601-01-01 UTC. Compute relative.
    constexpr int64_t EPOCH_DIFF_100NS = 116444736000000000LL; // 1601→1970
    auto now = std::chrono::system_clock::now().time_since_epoch();
    int64_t now_100ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100
        + EPOCH_DIFF_100NS;
    int64_t delta = due_100ns - now_100ns;
    if (delta < 0) delta = 0;
    return std::chrono::ceil<std::chrono::milliseconds>(
        std::chrono::nanoseconds(delta * 100));
}

u64 utc_filetime_now() {
    constexpr u64 kEpochDiff100ns = 116444736000000000ULL;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const u64 ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return ns / 100u + kEpochDiff100ns;
}

// Order measured by s4_timer_apc_probe (Windows 11, x86 and x64, 100/100):
// the PTIMERAPCROUTINE is already pending whenever a wait on the timer
// returns, yet the issuer's own alertable wait on the timer returns
// WAIT_OBJECT_0, not WAIT_IO_COMPLETION. So: queue the APC without waking,
// signal the timer, then wake alertable waits. Signalling first let a waiter
// observe the timer with no APC queued yet.
void fire_waitable_timer(SyncManager& mgr, Handle h, u32 issuer_tid,
                         GuestAddr pfn, GuestAddr arg) {
    ThreadManager* threads =
        (pfn != 0 && issuer_tid != 0) ? mgr.thread_manager() : nullptr;
    bool queued = false;
    if (threads) {
        const u64 ft = utc_filetime_now();
        queued = threads->queue_apc3_for_thread_quiet(
            issuer_tid, pfn, arg, static_cast<u32>(ft),
            static_cast<u32>(ft >> 32));
    }
    (void)mgr.signal_waitable_timer(h);
    if (queued) threads->notify_apc_queued(issuer_tid);
}
} // namespace

u64 SyncManager::bump_waitable_timer_generation(Handle h) {
    u64 gen;
    {
        std::lock_guard lock(m_waitable_timer_mutex);
        gen = ++m_waitable_timer_gen[h];
    }
    // Retire any worker still parked on the previous arm immediately, rather
    // than leaving it asleep until its own deadline.
    m_waitable_timer_cv.notify_all();
    return gen;
}

u64 SyncManager::waitable_timer_generation(Handle h) const {
    std::lock_guard lock(m_waitable_timer_mutex);
    const auto it = m_waitable_timer_gen.find(h);
    return it == m_waitable_timer_gen.end() ? 0 : it->second;
}

bool SyncManager::waitable_timer_sleep(Handle h, u64 gen,
                                       std::chrono::milliseconds d) const {
    std::unique_lock lock(m_waitable_timer_mutex);
    auto current = [this, h]() -> u64 {
        const auto it = m_waitable_timer_gen.find(h);
        return it == m_waitable_timer_gen.end() ? 0 : it->second;
    };
    // wait_for returns on notify, timeout, or spuriously; the predicate makes
    // all three cases correct. Shutdown also releases every parked worker.
    const bool superseded = m_waitable_timer_cv.wait_for(
        lock, d, [&]() {
            return current() != gen ||
                   m_shutdown.load(std::memory_order_acquire);
        });
    return !superseded;
}

void impl_CreateWaitableTimerW(Marshaler& m, SyncManager& mgr) {
    (void)m.arg_ptr(0); // lpTimerAttributes
    bool manual_reset = m.arg(1) != 0;
    GuestAddr name_ptr = m.arg_ptr(2);
    std::string name;
    if (name_ptr != 0) {
        auto r = m.read_wide_string(name_ptr);
        if (r.is_ok()) name = r.value();
    }
    Handle h = mgr.create_event(
        manual_reset, /*initial=*/false, name, /*inheritable=*/false,
        make_sync_trace_context(
            m, "CreateWaitableTimerW", mgr.causal_trace_enabled()),
        /*waitable_timer=*/true);
    VITREO_TRACE("[sync] CreateWaitableTimerW(manual=%d, name=\"%s\") -> 0x%x\n",
                 manual_reset, name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

void impl_CreateWaitableTimerA(Marshaler& m, SyncManager& mgr) {
    (void)m.arg_ptr(0); // lpTimerAttributes
    bool manual_reset = m.arg(1) != 0;
    GuestAddr name_ptr = m.arg_ptr(2);
    std::string name;
    if (name_ptr != 0) {
        auto r = m.read_ansi_string(name_ptr);
        if (r.is_ok()) name = r.value();
    }
    Handle h = mgr.create_event(
        manual_reset, /*initial=*/false, name, /*inheritable=*/false,
        make_sync_trace_context(
            m, "CreateWaitableTimerA", mgr.causal_trace_enabled()),
        /*waitable_timer=*/true);
    VITREO_TRACE("[sync] CreateWaitableTimerA(manual=%d, name=\"%s\") -> 0x%x\n",
                 manual_reset, name.c_str(), h);
    m.set_return(static_cast<u64>(h));
}

void impl_CreateWaitableTimerExW(Marshaler& m, SyncManager& mgr) {
    (void)m.arg_ptr(0); // lpTimerAttributes
    GuestAddr name_ptr = m.arg_ptr(1);
    u32 flags = static_cast<u32>(m.arg(2)); // CREATE_WAITABLE_TIMER_MANUAL_RESET=1
    (void)m.arg(3); // dwDesiredAccess
    bool manual_reset = (flags & 0x1u) != 0;
    std::string name;
    if (name_ptr != 0) {
        auto r = m.read_wide_string(name_ptr);
        if (r.is_ok()) name = r.value();
    }
    Handle h = mgr.create_event(
        manual_reset, /*initial=*/false, name, /*inheritable=*/false,
        make_sync_trace_context(
            m, "CreateWaitableTimerExW", mgr.causal_trace_enabled()),
        /*waitable_timer=*/true);
    VITREO_TRACE("[sync] CreateWaitableTimerExW(name=\"%s\", flags=0x%x) -> 0x%x\n",
                 name.c_str(), flags, h);
    m.set_return(static_cast<u64>(h));
}

SyncManager::~SyncManager() { stop_waitable_timer_service(); }

void SyncManager::arm_waitable_timer(Handle h, u64 gen,
                                     std::chrono::milliseconds delay,
                                     i32 period_ms, GuestAddr pfn,
                                     GuestAddr arg, u32 issuer_tid) {
    {
        std::lock_guard lock(m_waitable_timer_mutex);
        if (!m_waitable_timer_thread_started) {
            m_waitable_timer_thread_started = true;
            m_waitable_timer_thread =
                std::thread([this]() { waitable_timer_service(); });
        }
        m_waitable_timer_queue.emplace(
            std::chrono::steady_clock::now() + delay,
            WaitableTimerArm{h, gen, period_ms, pfn, arg, issuer_tid});
    }
    // Wake the service: this arm may be earlier than the deadline it is
    // currently sleeping to.
    m_waitable_timer_cv.notify_all();
}

void SyncManager::waitable_timer_service() {
    std::vector<WaitableTimerArm> due;
    std::unique_lock lock(m_waitable_timer_mutex);
    for (;;) {
        if (m_waitable_timer_stop ||
            m_shutdown.load(std::memory_order_acquire)) {
            return;
        }
        if (m_waitable_timer_queue.empty()) {
            m_waitable_timer_cv.wait(lock);
            continue;
        }
        const auto next = m_waitable_timer_queue.begin()->first;
        if (std::chrono::steady_clock::now() < next) {
            // A new, earlier arm or a shutdown wakes this early; both are
            // handled by re-testing at the top.
            m_waitable_timer_cv.wait_until(lock, next);
            continue;
        }
        // Collect everything due, dropping arms a later SetWaitableTimer or
        // CancelWaitableTimer superseded (same generation test the per-arm
        // sleeper used).
        due.clear();
        const auto now = std::chrono::steady_clock::now();
        while (!m_waitable_timer_queue.empty() &&
               m_waitable_timer_queue.begin()->first <= now) {
            auto it = m_waitable_timer_queue.begin();
            WaitableTimerArm arm = it->second;
            m_waitable_timer_queue.erase(it);
            const auto gen_it = m_waitable_timer_gen.find(arm.h);
            const u64 current = gen_it == m_waitable_timer_gen.end()
                                    ? 0
                                    : gen_it->second;
            if (current != arm.gen) continue;  // superseded — drop it
            due.push_back(arm);
            if (arm.period_ms > 0) {
                m_waitable_timer_queue.emplace(
                    now + std::chrono::milliseconds(arm.period_ms), arm);
            }
        }
        // fire_waitable_timer signals an event and queues an APC; it runs no
        // guest code, so it is safe from this thread. Drop the lock first:
        // set_event takes other locks and a waiter may re-arm from its APC.
        lock.unlock();
        for (const WaitableTimerArm& arm : due) {
            fire_waitable_timer(*this, arm.h, arm.issuer_tid, arm.pfn,
                                arm.arg);
        }
        lock.lock();
    }
}

void SyncManager::stop_waitable_timer_service() {
    {
        std::lock_guard lock(m_waitable_timer_mutex);
        m_waitable_timer_stop = true;
        m_waitable_timer_queue.clear();
    }
    m_waitable_timer_cv.notify_all();
    if (m_waitable_timer_thread.joinable()) m_waitable_timer_thread.join();
}

static void schedule_timer_signal(SyncManager& mgr, Handle h,
                                  std::chrono::milliseconds delay,
                                  int32_t period_ms, GuestAddr pfn,
                                  GuestAddr arg, u32 issuer_tid) {
    // Learn SetWaitableTimer: a new set cancels the previous period and any
    // pending PTIMERAPCROUTINE. CancelWaitableTimer uses the same generation.
    const u64 gen = mgr.bump_waitable_timer_generation(h);

    if (delay.count() <= 0) {
        // Fire immediately on the calling thread — matches the previous
        // behaviour and avoids a queue round trip for due=0 pacing loops.
        fire_waitable_timer(mgr, h, issuer_tid, pfn, arg);
        if (period_ms > 0) {
            mgr.arm_waitable_timer(h, gen,
                                   std::chrono::milliseconds(period_ms),
                                   period_ms, pfn, arg, issuer_tid);
        }
        return;
    }
    mgr.arm_waitable_timer(h, gen, delay, period_ms, pfn, arg, issuer_tid);
}

static bool set_waitable_timer_common(Marshaler& m, SyncManager& mgr,
                                      const char* api, Handle h,
                                      GuestAddr due_ptr, int32_t period,
                                      GuestAddr pfn, GuestAddr arg) {
    if (period < 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return false;
    }
    int64_t due_100ns = 0;
    if (due_ptr != 0) {
        auto r = m.memory().read_u64(due_ptr);
        if (r.is_ok()) due_100ns = static_cast<int64_t>(r.value());
    }
    auto delay = parse_due_time(due_100ns);
    const u32 issuer_tid = ThreadManager::get_current_thread_id_static();
    VITREO_TRACE("[sync] %s(0x%x, due=%lld 100ns, period=%d, pfn=0x%llx) "
                 "delay=%lldms tid=%u\n",
                 api, h, (long long)due_100ns, period,
                 static_cast<unsigned long long>(pfn),
                 (long long)delay.count(), issuer_tid);
    if (std::getenv("VITREO_LOG_WAIT_KINDS") != nullptr) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1) < 40) {
            std::fprintf(stderr,
                         "[timer-diag] %s h=0x%x due=%lld period=%d pfn=0x%llx "
                         "delay=%lldms tid=%u\n",
                         api, h, (long long)due_100ns, period,
                         static_cast<unsigned long long>(pfn),
                         (long long)delay.count(), issuer_tid);
        }
    }
    if (!mgr.reset_waitable_timer(
            h, make_sync_trace_context(m, api, mgr.causal_trace_enabled()))) {
        m.set_return(0);
        return false;
    }
    schedule_timer_signal(mgr, h, delay, period, pfn, arg, issuer_tid);
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1u);
    return true;
}

void impl_SetWaitableTimer(Marshaler& m, SyncManager& mgr) {
    const Handle h = static_cast<Handle>(m.arg(0));
    const GuestAddr due_ptr = m.arg_ptr(1);
    const int32_t period = static_cast<int32_t>(m.arg(2));
    const GuestAddr pfn = m.arg_ptr(3);
    const GuestAddr arg = m.arg_ptr(4);
    (void)m.arg(5); // fResume
    (void)set_waitable_timer_common(m, mgr, "SetWaitableTimer", h, due_ptr,
                                    period, pfn, arg);
}

void impl_SetWaitableTimerEx(Marshaler& m, SyncManager& mgr) {
    const Handle h = static_cast<Handle>(m.arg(0));
    const GuestAddr due_ptr = m.arg_ptr(1);
    const int32_t period = static_cast<int32_t>(m.arg(2));
    const GuestAddr pfn = m.arg_ptr(3);
    const GuestAddr arg = m.arg_ptr(4);
    (void)m.arg_ptr(5); // WakeContext
    (void)m.arg(6);     // TolerableDelay
    (void)set_waitable_timer_common(m, mgr, "SetWaitableTimerEx", h, due_ptr,
                                    period, pfn, arg);
}

void impl_CancelWaitableTimer(Marshaler& m, SyncManager& mgr) {
    // Learn CancelWaitableTimer: sets the timer inactive and cancels pending
    // APCs; it does not change the timer's signaled state.
    Handle h = static_cast<Handle>(m.arg(0));
    if (!mgr.is_waitable_timer(h)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    (void)mgr.bump_waitable_timer_generation(h);
    VITREO_TRACE("[sync] CancelWaitableTimer(0x%x) -> 1\n", h);
    m.set_return(1u);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_sync_thunks(SyncManager& mgr) {
    DllExports exports;
    exports.entries.reserve(34);

    // Critical-section exports are owned by CriticalSectionManager, which
    // maintains the Windows-visible RTL_CRITICAL_SECTION layout. Registering
    // host-only duplicates here silently shadowed that implementation.
    exports.entries.push_back(ExportEntry{
        "CreateMutexW", 7,
        [&mgr](Marshaler& m) { impl_CreateMutexW(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "CreateMutexA", 0,
        [&mgr](Marshaler& m) { impl_CreateMutexA(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "ReleaseMutex", 8,
        [&mgr](Marshaler& m) { impl_ReleaseMutex(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CreateEventW", 9,
        [&mgr](Marshaler& m) { impl_CreateEventW(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "CreateEventA", 0,
        [&mgr](Marshaler& m) { impl_CreateEventA(m, mgr); },
        16
    });
    exports.entries.push_back({"CreateEventExA",0,[&mgr](Marshaler& m){impl_SyncEx(m,mgr,false,false);},16});
    exports.entries.push_back({"CreateMutexExA",0,[&mgr](Marshaler& m){impl_SyncEx(m,mgr,true,false);},16});
    exports.entries.push_back({"CreateMutexExW",0,[&mgr](Marshaler& m){impl_SyncEx(m,mgr,true,true);},16});
    exports.entries.push_back(ExportEntry{
        "CreateEventExW", 0,
        [&mgr](Marshaler& m) { impl_CreateEventExW(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "OpenEventW", 0,
        [&mgr](Marshaler& m) { impl_OpenEventW(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "OpenEventA", 0,
        [&mgr](Marshaler& m) { impl_OpenEventA(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "SetEvent", 10,
        [&mgr](Marshaler& m) { impl_SetEvent(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "ResetEvent", 11,
        [&mgr](Marshaler& m) { impl_ResetEvent(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "PulseEvent", 0,
        [&mgr](Marshaler& m) { impl_PulseEvent(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "WaitForSingleObject", 12,
        [&mgr](Marshaler& m) { impl_WaitForSingleObject(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "SignalObjectAndWait", 0,
        [&mgr](Marshaler& m) { impl_SignalObjectAndWait(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "WaitForMultipleObjects", 13,
        [&mgr](Marshaler& m) { impl_WaitForMultipleObjects(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "WaitForMultipleObjectsEx", 0,
        [&mgr](Marshaler& m) { impl_WaitForMultipleObjectsEx(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "Sleep", 14,
        [&mgr](Marshaler& m) { impl_Sleep(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "SleepEx", 15,
        [&mgr](Marshaler& m) { impl_SleepEx(m, mgr); },
        8
    });

    // Waitable timers — stubbed via the event machinery.
    exports.entries.push_back(ExportEntry{
        "CreateWaitableTimerW", 0,
        [&mgr](Marshaler& m) { impl_CreateWaitableTimerW(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "CreateWaitableTimerA", 0,
        [&mgr](Marshaler& m) { impl_CreateWaitableTimerA(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "CreateWaitableTimerExW", 0,
        [&mgr](Marshaler& m) { impl_CreateWaitableTimerExW(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "SetWaitableTimer", 0,
        [&mgr](Marshaler& m) { impl_SetWaitableTimer(m, mgr); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "SetWaitableTimerEx", 0,
        [&mgr](Marshaler& m) { impl_SetWaitableTimerEx(m, mgr); },
        28
    });
    exports.entries.push_back(ExportEntry{
        "CancelWaitableTimer", 0,
        [&mgr](Marshaler& m) { impl_CancelWaitableTimer(m, mgr); },
        4
    });

    // WaitOnAddress / WakeByAddress* (Win8+; api-ms-win-core-synch).
    exports.entries.push_back(ExportEntry{
        "WaitOnAddress", 0,
        [](Marshaler& m) { impl_WaitOnAddress(m); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "WakeByAddressSingle", 0,
        [](Marshaler& m) { impl_WakeByAddressSingle(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "WakeByAddressAll", 0,
        [](Marshaler& m) { impl_WakeByAddressAll(m); },
        4
    });

    // OpenMutexW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "OpenMutexW", 0,
        [&mgr](Marshaler& m) { impl_OpenMutexW(m, mgr); },
        12
    });
    // OpenMutexA (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "OpenMutexA", 0,
        [&mgr](Marshaler& m) { impl_OpenMutexA(m, mgr); },
        12
    });
    // OpenSemaphoreW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "OpenSemaphoreW", 0,
        [&mgr](Marshaler& m) { impl_OpenSemaphoreW(m, mgr); },
        12
    });
    // OpenSemaphoreA (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "OpenSemaphoreA", 0,
        [&mgr](Marshaler& m) { impl_OpenSemaphoreA(m, mgr); },
        12
    });

    // InterlockedExchange (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "InterlockedExchange", 0,
        [](Marshaler& m) {
            // LONG InterlockedExchange(LONG volatile *Target, LONG Value)
            GuestAddr target = m.arg_ptr(0);
            u32 value = static_cast<u32>(m.arg(1));
            // Lock-free fast path: atomic xchg directly on host memory.
            if (auto* hp = m.memory().host_ptr_mut(target, 4)) {
                auto* p = reinterpret_cast<u32*>(hp);
                u32 old_value = __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST);
                m.set_return(old_value);
                return;
            }
            // Fallback for non-contiguous mappings (rare).
            std::lock_guard<std::mutex> lock(g_interlocked_api_mutex);
            auto old_result = m.read_u32(target);
            u32 old_value = old_result.is_ok() ? old_result.value() : 0;
            (void)m.write_u32(target, value);
            m.set_return(old_value);
        },
        8
    });

    // InterlockedCompareExchange (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "InterlockedCompareExchange", 0,
        [](Marshaler& m) {
            // LONG InterlockedCompareExchange(
            //     LONG volatile *Destination, LONG Exchange, LONG Comperand)
            // Returns the original *Destination. Swaps in Exchange iff
            // *Destination == Comperand.
            GuestAddr dest = m.arg_ptr(0);
            u32 exchange = static_cast<u32>(m.arg(1));
            u32 comperand = static_cast<u32>(m.arg(2));
            if (auto* hp = m.memory().host_ptr_mut(dest, 4)) {
                auto* p = reinterpret_cast<u32*>(hp);
                u32 expected = comperand;
                (void)__atomic_compare_exchange_n(
                    p, &expected, exchange,
                    /*weak=*/false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                m.set_return(expected);
                return;
            }
            std::lock_guard<std::mutex> lock(g_interlocked_api_mutex);
            auto old_result = m.read_u32(dest);
            u32 old_value = old_result.is_ok() ? old_result.value() : 0;
            if (old_value == comperand) {
                (void)m.write_u32(dest, exchange);
            }
            m.set_return(old_value);
        },
        12
    });

    // InterlockedIncrement (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "InterlockedIncrement", 0,
        [](Marshaler& m) {
            // LONG InterlockedIncrement(LONG volatile *Addend) -> new value
            GuestAddr target = m.arg_ptr(0);
            if (auto* hp = m.memory().host_ptr_mut(target, 4)) {
                auto* p = reinterpret_cast<u32*>(hp);
                u32 new_value = __atomic_add_fetch(p, 1u, __ATOMIC_SEQ_CST);
                m.set_return(new_value);
                return;
            }
            std::lock_guard<std::mutex> lock(g_interlocked_api_mutex);
            auto old_result = m.read_u32(target);
            u32 new_value = (old_result.is_ok() ? old_result.value() : 0) + 1;
            (void)m.write_u32(target, new_value);
            m.set_return(new_value);
        },
        4
    });

    // InterlockedDecrement (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "InterlockedDecrement", 0,
        [](Marshaler& m) {
            // LONG InterlockedDecrement(LONG volatile *Addend) -> new value
            GuestAddr target = m.arg_ptr(0);
            if (auto* hp = m.memory().host_ptr_mut(target, 4)) {
                auto* p = reinterpret_cast<u32*>(hp);
                u32 new_value = __atomic_sub_fetch(p, 1u, __ATOMIC_SEQ_CST);
                m.set_return(new_value);
                return;
            }
            std::lock_guard<std::mutex> lock(g_interlocked_api_mutex);
            auto old_result = m.read_u32(target);
            u32 new_value = (old_result.is_ok() ? old_result.value() : 0) - 1;
            (void)m.write_u32(target, new_value);
            m.set_return(new_value);
        },
        4
    });

    return exports;
}

} // namespace vitreo::winapi
