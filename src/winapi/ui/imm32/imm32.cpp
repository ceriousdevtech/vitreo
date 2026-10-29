#include "imm32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include <atomic>
#include <cstdio>

namespace vitreo::winapi {

namespace {

// Learn: ImmAssociateContextEx association flags.
constexpr u32 kIaceChildren = 0x0001;
constexpr u32 kIaceDefault = 0x0010;
constexpr u32 kIaceIgnoreNoContext = 0x0004;

} // namespace

Imm32Manager::Imm32Manager()
    : m_default_himc(0xE11C0001u)
    , m_next_himc(0xE11C0002u) {
    m_himcs.insert(m_default_himc);
    m_contexts.emplace(m_default_himc, Context{});
}

// Caller holds m_mutex (is_live / himc_for_hwnd / allocate_himc are the
// table operations the locked entry points compose).
bool Imm32Manager::is_live(u32 himc) const {
    return himc != 0 && m_himcs.contains(himc);
}

// Caller holds m_mutex.
u32 Imm32Manager::himc_for_hwnd(u32 hwnd) const {
    const auto it = m_hwnd_himc.find(hwnd);
    if (it != m_hwnd_himc.end()) return it->second;
    return m_default_himc;
}

// Caller holds m_mutex.
u32 Imm32Manager::allocate_himc() {
    while (m_himcs.contains(m_next_himc) || m_next_himc == 0) {
        ++m_next_himc;
        if (m_next_himc == 0) m_next_himc = 0xE11C0002u;
    }
    const u32 himc = m_next_himc++;
    m_himcs.insert(himc);
    m_contexts.emplace(himc, Context{});
    return himc;
}

bool Imm32Manager::with_context(u32 himc, const std::function<void(Context&)>& fn) {
    std::lock_guard lock(m_mutex);
    if (!is_live(himc)) return false;
    fn(m_contexts[himc]);
    return true;
}

void Imm32Manager::impl_ImmGetOpenStatus(Marshaler& m) {
    // Windows sets the last error to 0 whenever it locks a valid input
    // context (s2msg_input_probe imm* err=0 lines).
    const u32 himc = static_cast<u32>(m.arg(0));
    bool open = false;
    if (with_context(himc, [&](Context& c) { open = c.open; })) set_last_error(0);
    VITREO_TRACE("[imm32] ImmGetOpenStatus(himc=0x%x) -> %d\n", himc, open ? 1 : 0);
    m.set_return(open ? 1u : 0u);
}

void Imm32Manager::impl_ImmGetContext(Marshaler& m) {
    // Learn: ImmGetContext returns the HIMC associated with the window.
    // Newly created windows use the thread default context when none was
    // associated explicitly — never NULL for a normal desktop window.
    const u32 hwnd = static_cast<u32>(m.arg(0));
    // There is no context without a window: ImmGetContext(NULL) is NULL, and
    // callers use that to decide whether an IME is available at all.
    if (hwnd == 0) {
        m.set_return(0);
        return;
    }
    // immgetcontext_invalid_hwnd: NULL / ERROR_INVALID_WINDOW_HANDLE.
    if (m_window_valid && !m_window_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }
    u32 himc = 0;
    {
        std::lock_guard lock(m_mutex);
        himc = himc_for_hwnd(hwnd);
    }
    if (m_get_context_trace_count < 4) {
        VITREO_TRACE("[imm32] ImmGetContext(hwnd=0x%x) -> 0x%x\n", hwnd, himc);
        ++m_get_context_trace_count;
    }
    m.set_return(himc);
}

void Imm32Manager::impl_ImmReleaseContext(Marshaler& m) {
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const u32 himc = static_cast<u32>(m.arg(1));
    (void)hwnd;
    bool live = false;
    {
        std::lock_guard lock(m_mutex);
        live = is_live(himc);
    }
    if (!live) {
        VITREO_TRACE("[imm32] ImmReleaseContext(himc=0x%x) -> FALSE\n", himc);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Imm32Manager::impl_ImmAssociateContext(Marshaler& m) {
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const u32 himc = static_cast<u32>(m.arg(1));
    // immassociatecontext_invalid_hwnd: NULL / ERROR_INVALID_WINDOW_HANDLE.
    if (m_window_valid && !m_window_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }
    std::lock_guard lock(m_mutex);
    const u32 previous = himc_for_hwnd(hwnd);

    if (himc == 0) {
        m_hwnd_himc.erase(hwnd);
    } else if (!is_live(himc)) {
        VITREO_TRACE(
            "[imm32] ImmAssociateContext(hwnd=0x%x himc=0x%x) -> 0 "
            "(unknown HIMC)\n",
            hwnd, himc);
        m.set_return(0);
        return;
    } else {
        m_hwnd_himc[hwnd] = himc;
    }

    VITREO_TRACE(
        "[imm32] ImmAssociateContext(hwnd=0x%x himc=0x%x) -> prev=0x%x\n",
        hwnd, himc, previous);
    m.set_return(previous);
}

void Imm32Manager::impl_ImmAssociateContextEx(Marshaler& m) {
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const u32 himc = static_cast<u32>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));

    (void)kIaceChildren;
    (void)kIaceIgnoreNoContext;

    std::lock_guard lock(m_mutex);
    if ((flags & kIaceDefault) != 0 || himc == 0) {
        m_hwnd_himc.erase(hwnd);
    } else if (!is_live(himc)) {
        VITREO_TRACE(
            "[imm32] ImmAssociateContextEx(hwnd=0x%x himc=0x%x flags=0x%x) "
            "-> FALSE\n",
            hwnd, himc, flags);
        m.set_return(0);
        return;
    } else {
        m_hwnd_himc[hwnd] = himc;
    }

    VITREO_TRACE(
        "[imm32] ImmAssociateContextEx(hwnd=0x%x himc=0x%x flags=0x%x)%s "
        "-> TRUE\n",
        hwnd, himc, flags,
        (flags & kIaceDefault) ? " IACE_DEFAULT" : "");
    m.set_return(1);
}

void Imm32Manager::impl_ImmGetDefaultIMEWnd(Marshaler& m) {
    const u32 hwnd = static_cast<u32>(m.arg(0));
    VITREO_TRACE(
        "[imm32] ImmGetDefaultIMEWnd(hwnd=0x%x) -> NULL (no IME UI)\n", hwnd);
    (void)hwnd;
    m.set_return(0);
}

void Imm32Manager::impl_ImmCreateContext(Marshaler& m) {
    u32 himc = 0;
    {
        std::lock_guard lock(m_mutex);
        himc = allocate_himc();
    }
    VITREO_TRACE("[imm32] ImmCreateContext -> 0x%x\n", himc);
    m.set_return(himc);
}

void Imm32Manager::impl_ImmDestroyContext(Marshaler& m) {
    // A context that is not live fails with ERROR_INVALID_HANDLE
    // (immdestroycontext_again); success sets the last error to 0.
    const u32 himc = static_cast<u32>(m.arg(0));
    std::lock_guard lock(m_mutex);
    if (himc == m_default_himc || !is_live(himc)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        VITREO_TRACE("[imm32] ImmDestroyContext(himc=0x%x) -> FALSE\n", himc);
        m.set_return(0);
        return;
    }
    m_himcs.erase(himc);
    m_contexts.erase(himc);
    for (auto it = m_hwnd_himc.begin(); it != m_hwnd_himc.end();) {
        if (it->second == himc) it = m_hwnd_himc.erase(it);
        else ++it;
    }
    set_last_error(0);
    VITREO_TRACE("[imm32] ImmDestroyContext(himc=0x%x) -> TRUE\n", himc);
    m.set_return(1);
}


DllExports create_imm32_thunks(Imm32Manager& mgr) {
    DllExports exports;
    exports.entries.reserve(24);
    for (const char* name : {"ImmIsUIMessageA", "ImmIsUIMessageW"}) {
        exports.entries.push_back({name, 0, [](Marshaler& m) {
            const u32 message = static_cast<u32>(m.arg(1));
            // NULL asks only for classification. No IME window is currently
            // created by this manager, so a non-null window processes nothing.
            const bool ime_ui = (message >= 0x10d && message <= 0x10f) ||
                                (message >= 0x281 && message <= 0x286);
            m.set_return(m.arg_ptr(0) == 0 && ime_ui ? 1u : 0u);
        }, 16});
    }


    exports.entries.push_back(ExportEntry{
        "ImmGetOpenStatus", 0,
        [&mgr](Marshaler& m) { mgr.impl_ImmGetOpenStatus(m); },
        4
    });

    exports.entries.push_back(ExportEntry{
        // Learn (ImmGetDescriptionW): copies the IME's description and returns
        // the character count, or 0 when the HKL names no IME. Vitreo exposes
        // no IME, so 0 with an empty buffer is the accurate answer — the same
        // one a Windows machine with no IME installed gives.
        "ImmGetDescriptionW", 0,
        [](Marshaler& m) {
            const GuestAddr buffer = m.arg_ptr(1);
            const u32 chars = static_cast<u32>(m.arg(2));
            if (buffer != 0 && chars >= 1) {
                (void)m.write_u16(buffer, 0);
            }
            m.set_return(0);
        },
        12
    });

    exports.entries.push_back(ExportEntry{
        "ImmGetContext", 0,
        [&mgr](Marshaler& m) { mgr.impl_ImmGetContext(m); },
        4
    });

    // UINT ImmGetVirtualKey(HWND hWnd)
    //
    // Learn: https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immgetvirtualkey
    // Recovers the original virtual key behind a VK_PROCESSKEY message. Vitreo
    // never posts VK_PROCESSKEY — no IME consumes guest key input — so there is
    // no original key to recover, which the documentation spells VK_PROCESSKEY.
    exports.entries.push_back(ExportEntry{
        "ImmGetVirtualKey", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            constexpr u32 VK_PROCESSKEY = 0xE5;
            m.set_return(VK_PROCESSKEY);
        },
        4
    });

    exports.entries.push_back(ExportEntry{
        "ImmReleaseContext", 0,
        [&mgr](Marshaler& m) { mgr.impl_ImmReleaseContext(m); },
        8
    });

    exports.entries.push_back(ExportEntry{
        "ImmAssociateContext", 0,
        [&mgr](Marshaler& m) { mgr.impl_ImmAssociateContext(m); },
        8
    });

    exports.entries.push_back(ExportEntry{
        "ImmAssociateContextEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_ImmAssociateContextEx(m); },
        12
    });

    exports.entries.push_back(ExportEntry{
        "ImmGetDefaultIMEWnd", 0,
        [&mgr](Marshaler& m) { mgr.impl_ImmGetDefaultIMEWnd(m); },
        4
    });

    // ImmGetCompositionStringW(hIMC, dwIndex, lpBuf, dwBufLen) -> 0
    // LONG ImmGetCompositionStringW(HIMC, DWORD dwIndex, LPVOID, DWORD):
    // no IME ever composes, so every GCS_* string is empty (0); an index
    // that is not a GCS_* value is IMM_ERROR_GENERAL (-2)
    // (immgetcompositionstringw_len / _bad_index / _after_set).
    exports.entries.push_back(ExportEntry{
        "ImmGetCompositionStringW", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const u32 index = static_cast<u32>(m.arg(1));
            constexpr u32 kGcsValid = 0x0001 | 0x0002 | 0x0008 | 0x0010 | 0x0020 |
                                      0x0080 | 0x0100 | 0x0200 | 0x0400 | 0x0800 |
                                      0x1000 | 0x2000;
            if (!mgr.with_context(himc, [](Imm32Manager::Context&) {})) {
                m.set_return(0);
                return;
            }
            set_last_error(0);
            const bool single_bit = index != 0 && (index & (index - 1)) == 0;
            if (!single_bit || (index & kGcsValid) == 0) {
                constexpr i32 IMM_ERROR_GENERAL = -2;
                m.set_return(static_cast<u64>(static_cast<u32>(IMM_ERROR_GENERAL)));
                return;
            }
            m.set_return(0);
        },
        16
    });

    // ImmSetCompositionStringW(hIMC, dwIndex, lpComp, dwCompLen, lpRead, dwReadLen) -> FALSE
    // BOOL ImmSetCompositionStringW(...): accepted (TRUE, last error 0) and
    // nothing is composed -- the string does not stick without an IME
    // (immsetcompositionstringw / immgetcompositionstringw_after_set = 0).
    exports.entries.push_back(ExportEntry{
        "ImmSetCompositionStringW", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            (void)m.arg(1); (void)m.arg_ptr(2); (void)m.arg(3);
            (void)m.arg_ptr(4); (void)m.arg(5);
            const bool live = mgr.with_context(himc, [](Imm32Manager::Context&) {});
            if (live) set_last_error(0);
            m.set_return(live ? 1u : 0u);
        },
        24
    });

    // ImmGetCandidateListW(hIMC, dwIndex, lpCandList, dwBufLen) -> 0
    // ImmGetCandidateListW(hIMC, dwIndex, lpCandList, dwBufLen): no IME
    // offers candidates, so 0 bytes (immgetcandidatelistw).
    exports.entries.push_back(ExportEntry{
        "ImmGetCandidateListW", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            (void)m.arg(1); (void)m.arg_ptr(2); (void)m.arg(3);
            if (mgr.with_context(himc, [](Imm32Manager::Context&) {})) set_last_error(0);
            m.set_return(0);
        },
        16
    });

    // The composition font. The default is what Windows 11 reports for a
    // fresh en-US context -- 16 px, 7 wide, bold, ANSI_CHARSET,
    // VARIABLE_PITCH|FF_SWISS, "System" (s2msg_input_probe
    // immgetcompositionfontw); ImmSetCompositionFont replaces it.
    const auto default_font_w = [] {
        std::array<u8, 92> lf{};
        const auto put32 = [&lf](usize off, u32 v) {
            for (int i = 0; i < 4; ++i) lf[off + i] = static_cast<u8>(v >> (8 * i));
        };
        put32(0, 16);    // lfHeight
        put32(4, 7);     // lfWidth
        put32(16, 700);  // lfWeight
        lf[23] = 0;      // lfCharSet ANSI_CHARSET
        lf[27] = 34;     // lfPitchAndFamily VARIABLE_PITCH | FF_SWISS
        const char face[] = "System";
        for (usize i = 0; i < sizeof(face) - 1; ++i) lf[28 + i * 2] = static_cast<u8>(face[i]);
        return lf;
    };
    exports.entries.push_back(ExportEntry{
        "ImmSetCompositionFontW", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const GuestAddr lf_ptr = m.arg_ptr(1);
            std::array<u8, 92> lf{};
            if (lf_ptr == 0 || m.memory().read_bytes(lf_ptr, lf.data(), lf.size()).is_err()) {
                m.set_return(0);
                return;
            }
            const bool live = mgr.with_context(himc, [&](Imm32Manager::Context& c) {
                c.font_w = lf;
                c.font_set = true;
            });
            if (live) set_last_error(0);
            m.set_return(live ? 1u : 0u);
        },
        8
    });
    exports.entries.push_back(ExportEntry{
        "ImmGetCompositionFontW", 0,
        [&mgr, default_font_w](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const GuestAddr lf_ptr = m.arg_ptr(1);
            std::array<u8, 92> lf = default_font_w();
            const bool live = mgr.with_context(himc, [&](Imm32Manager::Context& c) {
                if (c.font_set) lf = c.font_w;
            });
            if (!live || lf_ptr == 0 ||
                m.memory().write_bytes(lf_ptr, lf.data(), lf.size()).is_err()) {
                m.set_return(0);
                return;
            }
            set_last_error(0);
            m.set_return(1);
        },
        8
    });
    // The ANSI forms carry a LOGFONTA. Callers import only the W pair.
    for (const char* name : {"ImmSetCompositionFontA", "ImmGetCompositionFontA"}) {
        exports.entries.push_back({name, 0, [name](Marshaler& m) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED imm32!%s\n", name);
            }
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            m.set_return(0);
        }, 8});
    }

    // ImmNotifyIME(hIMC, dwAction, dwIndex, dwValue) -> FALSE
    // BOOL ImmNotifyIME(hIMC, dwAction, dwIndex, dwValue): the documented
    // NI_* actions are accepted -- there is no composition to change -- and
    // anything else fails, both with the last error 0
    // (immnotifyime_cancel / immnotifyime_bad_action).
    exports.entries.push_back(ExportEntry{
        "ImmNotifyIME", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const u32 action = static_cast<u32>(m.arg(1));
            (void)m.arg(2); (void)m.arg(3);
            if (!mgr.with_context(himc, [](Imm32Manager::Context&) {})) {
                m.set_return(0);
                return;
            }
            set_last_error(0);
            // NI_OPENCANDIDATE .. NI_SETCANDIDATE_PAGESIZE (0x10..0x17),
            // NI_COMPOSITIONSTR (0x15), NI_FINALIZECONVERSIONRESULT (0x14).
            const bool known = action >= 0x10 && action <= 0x17;
            m.set_return(known ? 1u : 0u);
        },
        16
    });

    // ImmSetCompositionWindow(hIMC, lpCompForm) -> FALSE
    // BOOL ImmSetCompositionWindow(HIMC, LPCOMPOSITIONFORM) / the Get pair:
    // stored per context (immgetcompositionwindow style=2 pos=10,20).
    exports.entries.push_back(ExportEntry{
        "ImmSetCompositionWindow", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const GuestAddr form = m.arg_ptr(1);
            if (form == 0) {
                m.set_return(0);
                return;
            }
            const u32 style = m.read_u32(form).value_or(0);
            const i32 x = static_cast<i32>(m.read_u32(form + 4).value_or(0));
            const i32 y = static_cast<i32>(m.read_u32(form + 8).value_or(0));
            i32 rect[4];
            for (int i = 0; i < 4; ++i) {
                rect[i] = static_cast<i32>(m.read_u32(form + 12 + i * 4).value_or(0));
            }
            const bool live = mgr.with_context(himc, [&](Imm32Manager::Context& c) {
                c.comp_form_set = true;
                c.comp_style = style;
                c.comp_x = x;
                c.comp_y = y;
                for (int i = 0; i < 4; ++i) c.comp_rect[i] = rect[i];
            });
            if (live) set_last_error(0);
            m.set_return(live ? 1u : 0u);
        },
        8
    });
    exports.entries.push_back(ExportEntry{
        "ImmGetCompositionWindow", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const GuestAddr form = m.arg_ptr(1);
            bool set = false;
            Imm32Manager::Context copy{};
            const bool live = mgr.with_context(himc, [&](Imm32Manager::Context& c) {
                set = c.comp_form_set;
                copy = c;
            });
            if (!live || !set || form == 0) {
                if (live) set_last_error(0);
                m.set_return(0);
                return;
            }
            (void)m.write_u32(form, copy.comp_style);
            (void)m.write_u32(form + 4, static_cast<u32>(copy.comp_x));
            (void)m.write_u32(form + 8, static_cast<u32>(copy.comp_y));
            for (int i = 0; i < 4; ++i) {
                (void)m.write_u32(form + 12 + i * 4, static_cast<u32>(copy.comp_rect[i]));
            }
            set_last_error(0);
            m.set_return(1);
        },
        8
    });

    // ImmSetCandidateWindow(hIMC, lpCandidate) -> FALSE
    // BOOL ImmSetCandidateWindow(HIMC, LPCANDIDATEFORM) / the Get pair: four
    // candidate windows per context; dwIndex >= 4 fails with the last error
    // untouched (immsetcandidatewindow_bad_index); an index never set reads
    // FALSE (immgetcandidatewindow_unset).
    exports.entries.push_back(ExportEntry{
        "ImmSetCandidateWindow", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const GuestAddr form = m.arg_ptr(1);
            if (form == 0) {
                m.set_return(0);
                return;
            }
            const u32 index = m.read_u32(form).value_or(0xFFFFFFFFu);
            if (index >= 4) {
                m.set_return(0);
                return;
            }
            const u32 style = m.read_u32(form + 4).value_or(0);
            const i32 x = static_cast<i32>(m.read_u32(form + 8).value_or(0));
            const i32 y = static_cast<i32>(m.read_u32(form + 12).value_or(0));
            i32 rect[4];
            for (int i = 0; i < 4; ++i) {
                rect[i] = static_cast<i32>(m.read_u32(form + 16 + i * 4).value_or(0));
            }
            const bool live = mgr.with_context(himc, [&](Imm32Manager::Context& c) {
                c.cand_set[index] = true;
                c.cand_style[index] = style;
                c.cand_x[index] = x;
                c.cand_y[index] = y;
                for (int i = 0; i < 4; ++i) c.cand_rect[index][i] = rect[i];
            });
            if (live) set_last_error(0);
            m.set_return(live ? 1u : 0u);
        },
        8
    });
    exports.entries.push_back(ExportEntry{
        "ImmGetCandidateWindow", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const u32 index = static_cast<u32>(m.arg(1));
            const GuestAddr form = m.arg_ptr(2);
            Imm32Manager::Context copy{};
            const bool live = mgr.with_context(himc, [&](Imm32Manager::Context& c) { copy = c; });
            if (!live || index >= 4 || form == 0 || !copy.cand_set[index]) {
                if (live) set_last_error(0);
                m.set_return(0);
                return;
            }
            (void)m.write_u32(form, index);
            (void)m.write_u32(form + 4, copy.cand_style[index]);
            (void)m.write_u32(form + 8, static_cast<u32>(copy.cand_x[index]));
            (void)m.write_u32(form + 12, static_cast<u32>(copy.cand_y[index]));
            for (int i = 0; i < 4; ++i) {
                (void)m.write_u32(form + 16 + i * 4, static_cast<u32>(copy.cand_rect[index][i]));
            }
            set_last_error(0);
            m.set_return(1);
        },
        12
    });

    // ImmGetIMEFileNameA(hKL, lpszFileName, uBufLen) -> 0 (no IME)
    exports.entries.push_back(ExportEntry{
        "ImmGetIMEFileNameA", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            GuestAddr buf = m.arg_ptr(1);
            u32 buf_len = static_cast<u32>(m.arg(2));
            if (buf && buf_len > 0) {
                (void)m.write_u8(buf, 0);
            }
            m.set_return(0);
        },
        12
    });

    // The runtime exposes the en-US keyboard layout without an associated
    // Input Method Editor. Windows reports no IME file for such a layout.
    exports.entries.push_back(ExportEntry{
        "ImmGetIMEFileNameW", 0,
        [](Marshaler& m) {
            const u64 hkl = m.arg(0);
            const GuestAddr buffer = m.arg_ptr(1);
            const u32 buffer_chars = static_cast<u32>(m.arg(2));
            if (buffer != 0 && buffer_chars > 0) {
                (void)m.write_u16(buffer, 0);
            }
            VITREO_TRACE(
                "[imm32] ImmGetIMEFileNameW(hkl=0x%llx) -> 0 "
                "(layout has no IME)\n",
                static_cast<unsigned long long>(hkl));
            m.set_return(0);
        },
        12
    });

    // ImmSetOpenStatus(hIMC, fOpen) -> BOOL (TRUE)
    // BOOL ImmSetOpenStatus(HIMC, BOOL): stored; ImmGetOpenStatus reads it
    // back (immgetopenstatus_after = 1).
    exports.entries.push_back(ExportEntry{
        "ImmSetOpenStatus", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const bool open = m.arg32(1) != 0;
            const bool live = mgr.with_context(himc, [&](Imm32Manager::Context& c) { c.open = open; });
            if (live) set_last_error(0);
            m.set_return(live ? 1u : 0u);
        },
        8
    });

    // ImmSetConversionStatus(hIMC, fdwConv, fdwSent) -> BOOL (TRUE)
    // BOOL ImmSetConversionStatus(HIMC, fdwConversion, fdwSentence): stored
    // (immgetconversionstatus_after conv=1 sent=0).
    exports.entries.push_back(ExportEntry{
        "ImmSetConversionStatus", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const u32 conversion = static_cast<u32>(m.arg(1));
            const u32 sentence = static_cast<u32>(m.arg(2));
            const bool live = mgr.with_context(himc, [&](Imm32Manager::Context& c) {
                c.conversion = conversion;
                c.sentence = sentence;
            });
            if (live) set_last_error(0);
            m.set_return(live ? 1u : 0u);
        },
        12
    });

    // ImmGetConversionStatus(hIMC, lpfdwConv, lpfdwSent) -> BOOL (writes zeros)
    // BOOL ImmGetConversionStatus(HIMC, LPDWORD, LPDWORD)
    exports.entries.push_back(ExportEntry{
        "ImmGetConversionStatus", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const GuestAddr cv = m.arg_ptr(1);
            const GuestAddr sn = m.arg_ptr(2);
            u32 conversion = 0;
            u32 sentence = 0;
            const bool live = mgr.with_context(himc, [&](Imm32Manager::Context& c) {
                conversion = c.conversion;
                sentence = c.sentence;
            });
            if (!live) {
                m.set_return(0);
                return;
            }
            if (cv) (void)m.write_u32(cv, conversion);
            if (sn) (void)m.write_u32(sn, sentence);
            set_last_error(0);
            m.set_return(1);
        },
        12
    });

    // ImmGetCandidateListCountW(hIMC, lpdwListCount) -> DWORD (0 lists, 0 size)
    // DWORD ImmGetCandidateListCountW(HIMC, LPDWORD lpdwListCount): no
    // candidate lists; Windows 11 still answers 0x90 bytes for the (empty)
    // candidate data (immgetcandidatelistcountw ret=90 lists=0).
    exports.entries.push_back(ExportEntry{
        "ImmGetCandidateListCountW", 0,
        [&mgr](Marshaler& m) {
            const u32 himc = static_cast<u32>(m.arg(0));
            const GuestAddr cnt = m.arg_ptr(1);
            if (!mgr.with_context(himc, [](Imm32Manager::Context&) {})) {
                m.set_return(0);
                return;
            }
            if (cnt) (void)m.write_u32(cnt, 0);
            set_last_error(0);
            m.set_return(0x90u);
        },
        8
    });

    exports.entries.push_back(ExportEntry{
        "ImmDestroyContext", 0,
        [&mgr](Marshaler& m) { mgr.impl_ImmDestroyContext(m); },
        4
    });

    exports.entries.push_back(ExportEntry{
        "ImmCreateContext", 0,
        [&mgr](Marshaler& m) { mgr.impl_ImmCreateContext(m); },
        0
    });

    // ImmGetProperty(hKL, fdwIndex) -> DWORD (0 — no IME properties)
    // DWORD ImmGetProperty(HKL, DWORD fdwIndex) for the en-US layout, which
    // has no IME: IGP_PROPERTY = IME_PROP_UNICODE | IME_PROP_AT_CARET,
    // IGP_GETIMEVERSION = IMEVER_0400, anything else 0
    // (s2msg_input_probe immgetproperty_*).
    exports.entries.push_back(ExportEntry{
        "ImmGetProperty", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            const u32 index = static_cast<u32>(m.arg(1));
            constexpr u32 IGP_GETIMEVERSION = 0xFFFFFFFCu;  // (DWORD)-4
            constexpr u32 IGP_PROPERTY = 0x00000004u;
            u32 value = 0;
            if (index == IGP_PROPERTY) value = 0x00090000u;
            else if (index == IGP_GETIMEVERSION) value = 0x00040000u;
            m.set_return(value);
        },
        8
    });

    return exports;
}

} // namespace vitreo::winapi
