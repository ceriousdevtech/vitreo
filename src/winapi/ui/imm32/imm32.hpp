#pragma once
/// @file imm32.hpp
/// @brief IMM32.dll Input Method Manager contracts

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace vitreo::winapi {

class Marshaler;

/// Tracks synthetic HIMC handles. Vitreo does not host an East-Asian IME, but
/// Windows still exposes a default input context per thread that windows are
/// associated with — ImmGetContext must return that handle, not NULL.
class Imm32Manager {
public:
    Imm32Manager();

    /// HWND validity (ImmGetContext / ImmAssociateContext fail with
    /// ERROR_INVALID_WINDOW_HANDLE for a bad window). Unset: every window is
    /// accepted.
    void set_window_validator(std::function<bool(u32)> validator) {
        m_window_valid = std::move(validator);
    }

    /// The per-context state an application reads back (Learn "Input
    /// Context"): open status, conversion/sentence modes, the composition
    /// window, the four candidate windows and the composition font. Vitreo
    /// hosts no IME, so nothing else ever changes it.
    struct Context {
        bool open{false};
        // The default context's modes on Windows 11 en-US:
        // IME_CMODE_NATIVE, IME_SMODE_PHRASEPREDICT (s2msg_input_probe
        // immgetconversionstatus conv=1 sent=8).
        u32 conversion{1};
        u32 sentence{8};
        bool comp_form_set{false};
        u32 comp_style{0};
        i32 comp_x{0}, comp_y{0};
        i32 comp_rect[4]{};
        bool cand_set[4]{};
        u32 cand_style[4]{};
        i32 cand_x[4]{}, cand_y[4]{};
        i32 cand_rect[4][4]{};
        bool font_set{false};
        std::array<u8, 92> font_w{};  ///< LOGFONTW as the guest set it
    };
    /// Run @p fn on the live context @p himc under the lock; false when the
    /// handle names no context.
    bool with_context(u32 himc, const std::function<void(Context&)>& fn);

    void impl_ImmGetOpenStatus(Marshaler& m);
    void impl_ImmGetContext(Marshaler& m);
    void impl_ImmReleaseContext(Marshaler& m);
    void impl_ImmAssociateContext(Marshaler& m);
    void impl_ImmAssociateContextEx(Marshaler& m);
    void impl_ImmGetDefaultIMEWnd(Marshaler& m);
    void impl_ImmCreateContext(Marshaler& m);
    void impl_ImmDestroyContext(Marshaler& m);

private:
    [[nodiscard]] bool is_live(u32 himc) const;
    [[nodiscard]] u32 himc_for_hwnd(u32 hwnd) const;
    u32 allocate_himc();

    std::function<bool(u32)> m_window_valid;
    mutable std::mutex m_mutex;
    std::unordered_map<u32, Context> m_contexts;
    u32 m_default_himc;
    u32 m_next_himc;
    std::unordered_set<u32> m_himcs;
    /// Explicit ImmAssociateContext mappings. Absent hwnd → default HIMC.
    std::unordered_map<u32, u32> m_hwnd_himc;
    u32 m_get_context_trace_count{0};
};

DllExports create_imm32_thunks(Imm32Manager& mgr);

} // namespace vitreo::winapi
