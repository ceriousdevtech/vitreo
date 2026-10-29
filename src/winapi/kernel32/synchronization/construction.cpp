// SPDX-License-Identifier: LGPL-2.1-only

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

/// @file construction.cpp
/// @brief Construction
///
/// Closed cut from sync.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Construction
// =============================================================================

SyncManager::SyncManager(HandleTable& handles)
    : m_handles(handles)
    , m_causal_trace_enabled(
          environment_flag_enabled("VITREO_SYNC_CAUSAL_TRACE")) {}

void SyncManager::initialize_event_causality(
    SyncEvent& event, bool manual_reset, bool initial_state,
    SyncTraceContext trace) {
    if (!m_causal_trace_enabled) return;

    auto causality = std::make_unique<EventCausality>();
    causality->manual_reset = manual_reset;
    causality->initial_state = initial_state;
    causality->created.sequence =
        m_causal_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    causality->created.api =
        trace.api.empty() ? "internal" : std::string(trace.api);
    causality->created.thread_id =
        ThreadManager::get_current_thread_id_static();
    causality->created.caller = trace.caller;
    causality->created.parents = trace.parents;
    causality->created.parent_count = trace.parent_count;
    event.causality = std::move(causality);
}

void SyncManager::record_event_transition(
    const std::shared_ptr<SyncEvent>& event, std::string_view kind,
    SyncTraceContext trace) {
    if (!m_causal_trace_enabled || !event || !event->causality) return;

    SyncTraceRecord record;
    record.sequence =
        m_causal_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    record.api = trace.api.empty() ? "internal" : std::string(trace.api);
    record.thread_id = ThreadManager::get_current_thread_id_static();
    record.caller = trace.caller;
    record.parents = trace.parents;
    record.parent_count = trace.parent_count;

    std::lock_guard lock(event->causality->mutex);
    event->causality->last_transition = std::move(record);
    event->causality->last_transition_kind = kind;
}

void SyncManager::trace_event_wait(
    Handle handle, u32 milliseconds, SyncTraceContext trace) {
    if (!m_causal_trace_enabled || milliseconds != INFINITE_WAIT) return;

    const CausalWaitKey key{
        ThreadManager::get_current_thread_id_static(), handle, trace.caller};
    {
        std::lock_guard lock(m_causal_wait_mutex);
        if (m_causal_wait_seen.size() >= 512 ||
            std::find(m_causal_wait_seen.begin(), m_causal_wait_seen.end(),
                      key) != m_causal_wait_seen.end()) {
            return;
        }
        m_causal_wait_seen.push_back(key);
    }

    std::shared_ptr<SyncEvent> event;
    {
        std::shared_lock lock(m_mutex);
        const auto found = m_events.find(handle);
        if (found == m_events.end()) return;
        event = found->second;
    }
    if (!event || !event->causality) return;

    SyncTraceRecord created;
    std::optional<SyncTraceRecord> transition;
    std::string transition_kind;
    bool manual_reset = false;
    bool initial_state = false;
    {
        std::lock_guard lock(event->causality->mutex);
        created = event->causality->created;
        transition = event->causality->last_transition;
        transition_kind = event->causality->last_transition_kind;
        manual_reset = event->causality->manual_reset;
        initial_state = event->causality->initial_state;
    }

    std::string state = "shared-unknown";
    if (event->nt_id != 0) {
        const auto snapshot = ntsync::global().snapshot(event->nt_id);
        if (snapshot.has_value()) {
            state = snapshot->signaled ? "signaled" : "unsignaled";
        } else {
            state = "missing";
        }
    }

    const std::string_view wait_api =
        trace.api.empty() ? std::string_view{"internal"} : trace.api;
    const std::string wait_parents =
        format_parent_chain(trace.parents, trace.parent_count);
    const std::string created_parents =
        format_parent_chain(created.parents, created.parent_count);
    if (transition.has_value()) {
        const std::string transition_parents = format_parent_chain(
            transition->parents, transition->parent_count);
        std::fprintf(
            stderr,
            "[sync-cause] pid=%d wait h=0x%x name='%s' state=%s "
            "manual=%d initial=%d wait={api=%.*s tid=%u caller=0x%llx "
            "parents=%s} created={seq=%llu api=%s tid=%u caller=0x%llx "
            "parents=%s} last={seq=%llu op=%s api=%s tid=%u caller=0x%llx "
            "parents=%s}\n",
            static_cast<int>(::getpid()), static_cast<unsigned>(handle),
            event->name.c_str(), state.c_str(), manual_reset ? 1 : 0,
            initial_state ? 1 : 0, static_cast<int>(wait_api.size()),
            wait_api.data(), key.thread_id,
            static_cast<unsigned long long>(trace.caller),
            wait_parents.c_str(),
            static_cast<unsigned long long>(created.sequence),
            created.api.c_str(), created.thread_id,
            static_cast<unsigned long long>(created.caller),
            created_parents.c_str(),
            static_cast<unsigned long long>(transition->sequence),
            transition_kind.c_str(), transition->api.c_str(),
            transition->thread_id,
            static_cast<unsigned long long>(transition->caller),
            transition_parents.c_str());
    } else {
        std::fprintf(
            stderr,
            "[sync-cause] pid=%d wait h=0x%x name='%s' state=%s "
            "manual=%d initial=%d wait={api=%.*s tid=%u caller=0x%llx "
            "parents=%s} created={seq=%llu api=%s tid=%u caller=0x%llx "
            "parents=%s} last={none}\n",
            static_cast<int>(::getpid()), static_cast<unsigned>(handle),
            event->name.c_str(), state.c_str(), manual_reset ? 1 : 0,
            initial_state ? 1 : 0, static_cast<int>(wait_api.size()),
            wait_api.data(), key.thread_id,
            static_cast<unsigned long long>(trace.caller),
            wait_parents.c_str(),
            static_cast<unsigned long long>(created.sequence),
            created.api.c_str(), created.thread_id,
            static_cast<unsigned long long>(created.caller),
            created_parents.c_str());
    }
}

CriticalSection* SyncManager::get_or_create_critical_section(GuestAddr cs_addr) {
    auto it = m_critical_sections.find(cs_addr);
    if (it == m_critical_sections.end()) {
        it = m_critical_sections.emplace(cs_addr, std::make_unique<CriticalSection>()).first;
    }
    return it->second.get();
}

ConditionVariable* SyncManager::get_or_create_condition_variable(GuestAddr cv_addr) {
    auto it = m_condition_variables.find(cv_addr);
    if (it == m_condition_variables.end()) {
        it = m_condition_variables.emplace(cv_addr, std::make_unique<ConditionVariable>()).first;
    }
    return it->second.get();
}

SrwLock* SyncManager::get_or_create_srw_lock(GuestAddr lock_addr) {
    auto it = m_srw_locks.find(lock_addr);
    if (it == m_srw_locks.end()) {
        it = m_srw_locks.emplace(lock_addr, std::make_unique<SrwLock>()).first;
    }
    return it->second.get();
}

} // namespace vitreo::winapi
