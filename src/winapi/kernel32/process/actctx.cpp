// SPDX-License-Identifier: LGPL-2.1-only

/// @file actctx.cpp
/// @brief kernel32 activation-context APIs (CreateActCtx / ActivateActCtx).
///
/// Microsoft Learn: CreateActCtxW, ActivateActCtx, DeactivateActCtx,
/// ReleaseActCtx, GetCurrentActCtx. HANDLE lifetime and activate/deactivate
/// pairing are tracked per thread. SxS assembly probing is not redirected
/// through the active context yet.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

namespace vitreo::winapi {
namespace {

constexpr u32 kActCtxHandleBase = 0xAC000001u;
constexpr u32 kActCtxKnownFlags = 0x1FFu;
constexpr u32 kDeactivateForceEarly = 0x1u;

struct ActCtxStackEntry {
    u32 handle{0};
    u32 cookie{0};
};

thread_local std::vector<ActCtxStackEntry> t_actctx_stack;
thread_local u32 t_next_cookie{1};

} // namespace

void Kernel32ExtManager::create_actctx(Marshaler& m, bool wide) {
    const GuestAddr info = m.arg_ptr(0);
    if (info == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(INVALID_HANDLE);
        return;
    }
    const u32 cb_size = static_cast<u32>(m.read_u32(info).value_or(0));
    if (cb_size < 8u) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(INVALID_HANDLE);
        return;
    }
    const u32 flags = static_cast<u32>(m.read_u32(info + 4).value_or(0));
    if ((flags & ~kActCtxKnownFlags) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(INVALID_HANDLE);
        return;
    }

    std::string source;
    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    if (cb_size >= 8u + pointer_size) {
        const GuestAddr source_ptr = m.read_ptr(info + 8).value_or(0);
        if (source_ptr != 0) {
            auto text = wide ? m.read_wide_string(source_ptr)
                             : m.read_ansi_string(source_ptr);
            if (text.is_ok()) source = std::move(text.value());
        }
    }

    u32 handle = 0;
    {
        std::lock_guard lock(m_actctx_mutex);
        handle = kActCtxHandleBase + m_next_actctx;
        ++m_next_actctx;
        m_actctxs.emplace(handle, ActivationContext{});
    }
    ipc::ipc_log("CreateActCtx%s flags=0x%x source='%s' -> 0x%x",
                 wide ? "W" : "A", flags, source.c_str(), handle);
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(handle);
}

void Kernel32ExtManager::impl_CreateActCtxA(Marshaler& m) {
    create_actctx(m, false);
}

void Kernel32ExtManager::impl_CreateActCtxW(Marshaler& m) {
    create_actctx(m, true);
}

void Kernel32ExtManager::impl_ActivateActCtx(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    const GuestAddr cookie_ptr = m.arg_ptr(1);
    if (cookie_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (handle != 0) {
        std::lock_guard lock(m_actctx_mutex);
        if (m_actctxs.find(handle) == m_actctxs.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
    }
    const u32 cookie = t_next_cookie++;
    t_actctx_stack.push_back(ActCtxStackEntry{handle, cookie});
    if (m.cpu_is_64bit()) {
        if (m.write_u64(cookie_ptr, cookie).is_err()) {
            t_actctx_stack.pop_back();
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
    } else if (m.write_u32(cookie_ptr, cookie).is_err()) {
        t_actctx_stack.pop_back();
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_DeactivateActCtx(Marshaler& m) {
    const u32 flags = static_cast<u32>(m.arg(0));
    const u32 cookie = static_cast<u32>(m.arg(1));
    if (t_actctx_stack.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if ((flags & kDeactivateForceEarly) != 0) {
        while (!t_actctx_stack.empty() &&
               t_actctx_stack.back().cookie != cookie) {
            t_actctx_stack.pop_back();
        }
    }
    if (t_actctx_stack.empty() || t_actctx_stack.back().cookie != cookie) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    t_actctx_stack.pop_back();
    m.set_return(1);
}

void Kernel32ExtManager::impl_ReleaseActCtx(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    if (handle == 0 || handle == INVALID_HANDLE) return;
    std::lock_guard lock(m_actctx_mutex);
    auto it = m_actctxs.find(handle);
    if (it == m_actctxs.end()) return;
    if (it->second.refs > 0) --it->second.refs;
    if (it->second.refs == 0) m_actctxs.erase(it);
}

void Kernel32ExtManager::impl_GetCurrentActCtx(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(0);
    if (out == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (t_actctx_stack.empty() || t_actctx_stack.back().handle == 0) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }
    const u32 handle = t_actctx_stack.back().handle;
    {
        std::lock_guard lock(m_actctx_mutex);
        auto it = m_actctxs.find(handle);
        if (it == m_actctxs.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        ++it->second.refs;
    }
    if (m.cpu_is_64bit()) {
        if (m.write_u64(out, handle).is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
    } else if (m.write_u32(out, handle).is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

} // namespace vitreo::winapi
