#include "comctl32.hpp"
#include "winapi/user32/windows/sdk_creation_layouts.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace vitreo::winapi {

thread_local std::vector<ComCtl32Manager::DispatchFrame*>
    ComCtl32Manager::s_dispatch_stack;

// =============================================================================
// ComCtl32Manager implementations
// =============================================================================

void ComCtl32Manager::impl_InitCommonControls(Marshaler& /*m*/) const {
    VITREO_TRACE( "[comctl32] InitCommonControls: no-op\n");
    // void return — nothing to do
}

u32 ComCtl32Manager::create_image_list(i32 cx, i32 cy, u32 flags,
                                       i32 initial, i32 grow) {
    const u32 handle = m_next_imglist_handle++;
    m_image_lists[handle] = ImageListState{cx, cy, flags, initial, grow, 0};
    return handle;
}

void ComCtl32Manager::impl_ImageList_Create(Marshaler& m) {
    auto cx = static_cast<i32>(m.arg(0));
    auto cy = static_cast<i32>(m.arg(1));
    auto flags = static_cast<u32>(m.arg(2));
    auto cInitial = static_cast<i32>(m.arg(3));
    auto cGrow = static_cast<i32>(m.arg(4));

    u32 handle = create_image_list(cx, cy, flags, cInitial, cGrow);

    VITREO_TRACE("[comctl32] ImageList_Create: cx=%d cy=%d flags=0x%x initial=%d grow=%d -> 0x%x\n",
                cx, cy, flags, cInitial, cGrow, handle);
    m.set_return(handle);
}

void ComCtl32Manager::impl_ImageList_AddMasked(Marshaler& m) {
    u32 himl = static_cast<u32>(m.arg(0));
    (void)m.arg(1); // hbmImage
    (void)m.arg(2); // crMask

    auto it = m_image_lists.find(himl);
    if (it == m_image_lists.end()) {
        VITREO_TRACE("[comctl32] ImageList_AddMasked: himl=0x%x -> 0 (not found)\n", himl);
        m.set_return(0u);
        return;
    }

    i32 index = it->second.count;
    it->second.count++;
    VITREO_TRACE("[comctl32] ImageList_AddMasked: himl=0x%x -> %d (count=%d)\n",
                himl, index, it->second.count);
    m.set_return(static_cast<u64>(static_cast<u32>(index)));
}

void ComCtl32Manager::impl_ImageList_Destroy(Marshaler& m) {
    u32 himl = static_cast<u32>(m.arg(0));
    auto erased = m_image_lists.erase(himl);
    VITREO_TRACE("[comctl32] ImageList_Destroy: himl=0x%x -> %s\n",
                himl, erased ? "TRUE" : "FALSE (not found)");
    m.set_return(erased ? 1u : 0u);
}

void ComCtl32Manager::impl_InitCommonControlsEx(Marshaler& m) {
    GuestAddr icc_ptr = m.arg_ptr(0);
    if (icc_ptr != 0) {
        auto size_res = m.read_u32(icc_ptr);
        auto icc_res = m.read_u32(icc_ptr + 4);
        u32 dw_size = size_res.is_ok() ? size_res.value() : 0;
        u32 dw_icc = icc_res.is_ok() ? icc_res.value() : 0;
        VITREO_TRACE("[comctl32] InitCommonControlsEx: dwSize=%u dwICC=0x%08x", dw_size, dw_icc);
        if (dw_icc & 0x01) VITREO_TRACE(" ICC_LISTVIEW");
        if (dw_icc & 0x02) VITREO_TRACE(" ICC_TREEVIEW");
        if (dw_icc & 0x04) VITREO_TRACE(" ICC_BAR");
        if (dw_icc & 0x08) VITREO_TRACE(" ICC_TAB");
        if (dw_icc & 0x10) VITREO_TRACE(" ICC_UPDOWN");
        if (dw_icc & 0x20) VITREO_TRACE(" ICC_PROGRESS");
        if (dw_icc & 0x40) VITREO_TRACE(" ICC_HOTKEY");
        if (dw_icc & 0x80) VITREO_TRACE(" ICC_ANIMATE");
        if (dw_icc & 0xFF) VITREO_TRACE(" ICC_WIN95");
        if (dw_icc & 0x100) VITREO_TRACE(" ICC_DATE");
        if (dw_icc & 0x200) VITREO_TRACE(" ICC_USEREX");
        if (dw_icc & 0x400) VITREO_TRACE(" ICC_COOL");
        if (dw_icc & 0x800) VITREO_TRACE(" ICC_INTERNET");
        if (dw_icc & 0x1000) VITREO_TRACE(" ICC_PAGESCROLLER");
        if (dw_icc & 0x2000) VITREO_TRACE(" ICC_NATIVEFNTCTL");
        if (dw_icc & 0x4000) VITREO_TRACE(" ICC_STANDARD");
        if (dw_icc & 0x8000) VITREO_TRACE(" ICC_LINK");
        VITREO_TRACE(" -> TRUE\n");
    } else {
        VITREO_TRACE("[comctl32] InitCommonControlsEx: NULL ptr -> FALSE\n");
        m.set_return(0);  // FALSE — NULL lpInitCtrls is invalid
        return;
    }
    m.set_return(1);  // TRUE
}

void ComCtl32Manager::impl_ImageList_GetIconSize(Marshaler& m) {
    u32 himl = static_cast<u32>(m.arg(0));
    GuestAddr cx_ptr = m.arg_ptr(1);
    GuestAddr cy_ptr = m.arg_ptr(2);

    auto it = m_image_lists.find(himl);
    if (it != m_image_lists.end()) {
        if (cx_ptr) (void)m.write_u32(cx_ptr, static_cast<u32>(it->second.cx));
        if (cy_ptr) (void)m.write_u32(cy_ptr, static_cast<u32>(it->second.cy));
        VITREO_TRACE("[comctl32] ImageList_GetIconSize: himl=0x%x cx=%d cy=%d -> TRUE\n",
                    himl, it->second.cx, it->second.cy);
    } else {
        if (cx_ptr) (void)m.write_u32(cx_ptr, 0);
        if (cy_ptr) (void)m.write_u32(cy_ptr, 0);
        VITREO_TRACE("[comctl32] ImageList_GetIconSize: himl=0x%x -> TRUE (not found)\n", himl);
    }
    m.set_return(1);  // TRUE
}

void ComCtl32Manager::impl_ImageList_SetIconSize(Marshaler& m) {
    // Learn: ImageList_SetIconSize(himl, cx, cy) — sets image dimensions and
    // removes all images from the list. Returns nonzero on success.
    u32 himl = static_cast<u32>(m.arg(0));
    auto cx = static_cast<i32>(m.arg(1));
    auto cy = static_cast<i32>(m.arg(2));

    auto it = m_image_lists.find(himl);
    if (it == m_image_lists.end()) {
        VITREO_TRACE("[comctl32] ImageList_SetIconSize: himl=0x%x -> FALSE\n", himl);
        m.set_return(0u);
        return;
    }

    it->second.cx = cx;
    it->second.cy = cy;
    it->second.count = 0;
    VITREO_TRACE("[comctl32] ImageList_SetIconSize: himl=0x%x cx=%d cy=%d -> TRUE\n",
                himl, cx, cy);
    m.set_return(1u);
}

void ComCtl32Manager::impl_ImageList_GetImageCount(Marshaler& m) {
    // Learn: ImageList_GetImageCount(himl) → int image count.
    u32 himl = static_cast<u32>(m.arg(0));
    auto it = m_image_lists.find(himl);
    i32 count = (it != m_image_lists.end()) ? it->second.count : 0;
    VITREO_TRACE("[comctl32] ImageList_GetImageCount: himl=0x%x -> %d\n", himl, count);
    m.set_return(static_cast<u64>(static_cast<u32>(count)));
}

void ComCtl32Manager::impl_ImageList_Draw(Marshaler& m) {
    // Learn: ImageList_Draw(himl, i, hdc, x, y, fStyle) → nonzero on success.
    u32 himl = static_cast<u32>(m.arg(0));
    (void)m.arg(1); // i
    (void)m.arg(2); // hdcDst
    (void)m.arg(3); // x
    (void)m.arg(4); // y
    (void)m.arg(5); // fStyle
    const bool ok = m_image_lists.find(himl) != m_image_lists.end();
    VITREO_TRACE("[comctl32] ImageList_Draw: himl=0x%x -> %s\n",
                himl, ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1u : 0u);
}

void ComCtl32Manager::impl_DllGetVersion(Marshaler& m) {
    GuestAddr pdvi = m.arg_ptr(0);
    if (pdvi == 0) {
        m.set_return(0x80070057u); // E_INVALIDARG
        return;
    }

    auto cb_size = m.read_u32(pdvi);
    if (cb_size.is_err()) {
        m.set_return(0x80004005u); // E_FAIL
        return;
    }

    // DLLVERSIONINFO / DLLVERSIONINFO2 common prefix
    // struct {
    //   DWORD cbSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformID;
    // }
    const u32 size = cb_size.value();
    if (size < 20) {
        m.set_return(0x80070057u); // E_INVALIDARG
        return;
    }

    (void)m.write_u32(pdvi + 4, 6);   // dwMajorVersion
    (void)m.write_u32(pdvi + 8, 10);  // dwMinorVersion
    (void)m.write_u32(pdvi + 12, 0);  // dwBuildNumber
    (void)m.write_u32(pdvi + 16, 2);  // dwPlatformID (DLLVER_PLATFORM_WINDOWS)

    VITREO_TRACE("[comctl32] DllGetVersion(pdvi=0x%llx, cbSize=%u) "
                 "-> S_OK (6.10)\n",
                 static_cast<unsigned long long>(pdvi), size);
    m.set_return(0); // S_OK
}

void ComCtl32Manager::impl_LoadIconMetric(Marshaler& m) {
    constexpr u32 S_OK = 0x00000000u;
    constexpr u32 E_INVALIDARG = 0x80070057u;
    constexpr u32 E_NOTIMPL = 0x80004001u;
    constexpr u32 HRESULT_RESOURCE_NAME_NOT_FOUND = 0x80070716u;
    constexpr u32 LIM_LARGE = 1;

    const GuestAddr instance = m.arg_ptr(0);
    const GuestAddr name = m.arg_ptr(1);
    const u32 metric = static_cast<u32>(m.arg(2));
    const GuestAddr icon_out = m.arg_ptr(3);

    if (icon_out == 0 || metric > LIM_LARGE) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (m.write_ptr(icon_out, 0).is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (m_resource_mgr == nullptr) {
        m.set_return(E_NOTIMPL);
        return;
    }

    // MAKEINTRESOURCE values occupy the low 16 bits. Named icon groups are a
    // valid Windows case, but Vitreo's current resource walker only assembles
    // group icons by integer ID; fail honestly rather than manufacture one.
    if (name == 0 || name >= 0x10000) {
        m.set_return(name == 0 ? E_INVALIDARG : E_NOTIMPL);
        return;
    }

    const u16 icon_id = static_cast<u16>(name);
    const u32 icon =
        m_resource_mgr->load_icon_metric(instance, icon_id, metric);
    if (icon == 0) {
        m.set_return(HRESULT_RESOURCE_NAME_NOT_FOUND);
        return;
    }
    if (m.write_ptr(icon_out, icon).is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }

    VITREO_TRACE(
        "[comctl32] LoadIconMetric hinst=0x%x id=%u metric=%u -> 0x%x\n",
        instance, static_cast<unsigned>(icon_id), metric, icon);
    m.set_return(S_OK);
}

bool ComCtl32Manager::is_calling_thread_owner(windowing::HWND hwnd) const {
    if (m_hwnd_mgr == nullptr) {
        return false;
    }
    const auto owner = m_hwnd_mgr->owner_thread_id(hwnd);
    return owner.has_value() &&
           *owner == ThreadManager::get_current_thread_id_static();
}

bool ComCtl32Manager::set_window_subclass(
    windowing::HWND hwnd, GuestAddr proc, u64 id, u64 ref_data
) {
    if (proc == 0 || !is_calling_thread_owner(hwnd)) {
        return false;
    }

    std::lock_guard lock(m_subclass_mutex);
    auto& chain = m_subclasses[hwnd];
    const auto existing = std::find_if(
        chain.begin(), chain.end(),
        [proc, id](const SubclassEntry& entry) {
            return entry.proc == proc && entry.id == id;
        });
    if (existing != chain.end()) {
        existing->ref_data = ref_data;
        return true;
    }

    chain.push_back(SubclassEntry{proc, id, ref_data});
    return true;
}

bool ComCtl32Manager::remove_window_subclass(
    windowing::HWND hwnd, GuestAddr proc, u64 id
) {
    if (proc == 0 || !is_calling_thread_owner(hwnd)) {
        return false;
    }

    std::lock_guard lock(m_subclass_mutex);
    const auto map_it = m_subclasses.find(hwnd);
    if (map_it == m_subclasses.end()) {
        return false;
    }

    auto& chain = map_it->second;
    const auto entry_it = std::find_if(
        chain.begin(), chain.end(),
        [proc, id](const SubclassEntry& entry) {
            return entry.proc == proc && entry.id == id;
        });
    if (entry_it == chain.end()) {
        return false;
    }

    chain.erase(entry_it);
    if (chain.empty()) {
        m_subclasses.erase(map_it);
    }
    return true;
}

bool ComCtl32Manager::get_window_subclass(
    windowing::HWND hwnd, GuestAddr proc, u64 id, u64& ref_data
) const {
    if (proc == 0 || !is_calling_thread_owner(hwnd)) {
        return false;
    }

    std::lock_guard lock(m_subclass_mutex);
    const auto map_it = m_subclasses.find(hwnd);
    if (map_it == m_subclasses.end()) {
        return false;
    }

    const auto entry_it = std::find_if(
        map_it->second.begin(), map_it->second.end(),
        [proc, id](const SubclassEntry& entry) {
            return entry.proc == proc && entry.id == id;
        });
    if (entry_it == map_it->second.end()) {
        return false;
    }

    ref_data = entry_it->ref_data;
    return true;
}

bool ComCtl32Manager::find_subclass(
    windowing::HWND hwnd, const SubclassKey& key, SubclassEntry& entry
) const {
    std::lock_guard lock(m_subclass_mutex);
    const auto map_it = m_subclasses.find(hwnd);
    if (map_it == m_subclasses.end()) {
        return false;
    }
    const auto entry_it = std::find_if(
        map_it->second.begin(), map_it->second.end(),
        [&key](const SubclassEntry& candidate) {
            return candidate.proc == key.proc && candidate.id == key.id;
        });
    if (entry_it == map_it->second.end()) {
        return false;
    }
    entry = *entry_it;
    return true;
}

u64 ComCtl32Manager::invoke_next(
    DispatchFrame& frame, windowing::HWND hwnd, u32 msg,
    u64 w_param, u64 l_param
) {
    if (!m_guest_call) {
        return 0;
    }

    while (frame.next < frame.order.size()) {
        const SubclassKey key = frame.order[frame.next++];
        SubclassEntry entry;
        // A callback removed before its turn must not be invoked. Reference
        // data updates made during dispatch are observed by later callbacks.
        if (!find_subclass(frame.hwnd, key, entry)) {
            continue;
        }

        const u64 args[6] = {
            static_cast<u64>(hwnd),
            static_cast<u64>(msg),
            w_param,
            l_param,
            entry.id,
            entry.ref_data,
        };
        VITREO_TRACE(
            "[comctl32] subclass hwnd=0x%x msg=0x%x proc=0x%llx "
            "id=0x%llx ref=0x%llx\n",
            frame.hwnd, msg,
            static_cast<unsigned long long>(entry.proc),
            static_cast<unsigned long long>(entry.id),
            static_cast<unsigned long long>(entry.ref_data));
        return m_guest_call(entry.proc, args, 6, frame.is_64bit);
    }

    if (frame.original_wndproc == 0) {
        return 0;
    }
    const u64 args[4] = {
        static_cast<u64>(hwnd),
        static_cast<u64>(msg),
        w_param,
        l_param,
    };
    return m_guest_call(
        frame.original_wndproc, args, 4, frame.is_64bit);
}

u64 ComCtl32Manager::dispatch_window_message(
    windowing::HWND hwnd, u32 msg, u64 w_param, u64 l_param,
    GuestAddr original_wndproc, bool is_64bit
) {
    DispatchFrame frame;
    frame.manager = this;
    frame.hwnd = hwnd;
    frame.original_wndproc = original_wndproc;
    frame.is_64bit = is_64bit;

    {
        std::lock_guard lock(m_subclass_mutex);
        const auto it = m_subclasses.find(hwnd);
        if (it != m_subclasses.end()) {
            frame.order.reserve(it->second.size());
            for (auto entry = it->second.rbegin();
                 entry != it->second.rend(); ++entry) {
                frame.order.push_back(SubclassKey{entry->proc, entry->id});
            }
        }
    }

    s_dispatch_stack.push_back(&frame);
    const u64 result = invoke_next(frame, hwnd, msg, w_param, l_param);
    s_dispatch_stack.pop_back();

    // comctl32 releases its per-window subclass property as the window
    // completes WM_NCDESTROY processing.
    constexpr u32 WM_NCDESTROY = 0x0082;
    if (msg == WM_NCDESTROY) {
        std::lock_guard lock(m_subclass_mutex);
        m_subclasses.erase(hwnd);
    }
    return result;
}

void ComCtl32Manager::impl_SetWindowSubclass(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr proc = m.arg_ptr(1);
    const u64 id = m.arg(2);
    const u64 ref_data = m.arg(3);
    const bool installed = set_window_subclass(hwnd, proc, id, ref_data);
    VITREO_TRACE(
        "[comctl32] SetWindowSubclass hwnd=0x%x proc=0x%llx "
        "id=0x%llx ref=0x%llx -> %s\n",
        hwnd, static_cast<unsigned long long>(proc),
        static_cast<unsigned long long>(id),
        static_cast<unsigned long long>(ref_data),
        installed ? "TRUE" : "FALSE");
    m.set_return(installed ? 1u : 0u);
}

void ComCtl32Manager::impl_GetWindowSubclass(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr proc = m.arg_ptr(1);
    const u64 id = m.arg(2);
    const GuestAddr ref_data_out = m.arg_ptr(3);
    if (ref_data_out == 0) {
        m.set_return(0);
        return;
    }

    u64 ref_data = 0;
    if (!get_window_subclass(hwnd, proc, id, ref_data) ||
        m.write_ptr(ref_data_out, ref_data).is_err()) {
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void ComCtl32Manager::impl_RemoveWindowSubclass(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr proc = m.arg_ptr(1);
    const u64 id = m.arg(2);
    const bool removed = remove_window_subclass(hwnd, proc, id);
    VITREO_TRACE(
        "[comctl32] RemoveWindowSubclass hwnd=0x%x proc=0x%llx "
        "id=0x%llx -> %s\n",
        hwnd, static_cast<unsigned long long>(proc),
        static_cast<unsigned long long>(id),
        removed ? "TRUE" : "FALSE");
    m.set_return(removed ? 1u : 0u);
}

void ComCtl32Manager::impl_DefSubclassProc(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u32 msg = static_cast<u32>(m.arg(1));
    const u64 w_param = m.arg(2);
    const u64 l_param = m.arg(3);

    if (s_dispatch_stack.empty() ||
        s_dispatch_stack.back()->manager != this) {
        VITREO_TRACE(
            "[comctl32] DefSubclassProc outside active subclass dispatch\n");
        m.set_return(0);
        return;
    }

    m.set_return(invoke_next(
        *s_dispatch_stack.back(), hwnd, msg, w_param, l_param));
}

// =============================================================================
// Thunk Registration
// =============================================================================

DllExports create_comctl32_thunks(ComCtl32Manager& mgr, ApiHandler track_mouse,
                                    ApiHandler set_scroll_info, FlatScrollHandlers scroll) {
    DllExports exports;
    exports.entries.reserve(12);
    // Microsoft Learn: _TrackMouseEvent delegates to User32 TrackMouseEvent.
    exports.entries.push_back({"_TrackMouseEvent", 0,
        [track_mouse = std::move(track_mouse)](Marshaler& m) {
            if (track_mouse) track_mouse(m);
            else {
                set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                m.set_return(0);
            }
        }, 4});


    // InitCommonControls — imported by ordinal #17
    exports.entries.push_back(ExportEntry{
        "InitCommonControls", 17,
        [&mgr](Marshaler& m) { mgr.impl_InitCommonControls(m); },
        0
    });

    // ImageList_Create (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "ImageList_Create", 1,
        [&mgr](Marshaler& m) {
            i32 cx = static_cast<i32>(m.arg(0));
            i32 cy = static_cast<i32>(m.arg(1));
            if (cx <= 0 || cy <= 0) {
                set_last_error(0x51Du /*ERROR_INVALID_PARAMETER_2*/);
                m.set_return(0);
                return;
            }
            mgr.impl_ImageList_Create(m);
        },
        20
    });

    // ImageList_AddMasked (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "ImageList_AddMasked", 2,
        [&mgr](Marshaler& m) { mgr.impl_ImageList_AddMasked(m); },
        12
    });

    // ImageList_Destroy (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "ImageList_Destroy", 3,
        [&mgr](Marshaler& m) { mgr.impl_ImageList_Destroy(m); },
        4
    });

    // InitCommonControlsEx (1 arg: LPINITCOMMONCONTROLSEX, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "InitCommonControlsEx", 4,
        [&mgr](Marshaler& m) { mgr.impl_InitCommonControlsEx(m); },
        4
    });

    // ImageList_GetIconSize (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "ImageList_GetIconSize", 5,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_ImageList_GetIconSize(m); },
        12
    });

    // ImageList_SetIconSize (3 args, stdcall_bytes=12) — Learn commctrl
    exports.entries.push_back(ExportEntry{
        "ImageList_SetIconSize", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0); return; }
            mgr.impl_ImageList_SetIconSize(m);
        },
        12
    });

    // ImageList_GetImageCount (1 arg, stdcall_bytes=4) — Learn commctrl
    exports.entries.push_back(ExportEntry{
        "ImageList_GetImageCount", 0,
        [&mgr](Marshaler& m) { mgr.impl_ImageList_GetImageCount(m); },
        4
    });

    // ImageList_Draw (6 args, stdcall_bytes=24) — Learn commctrl
    exports.entries.push_back(ExportEntry{
        "ImageList_Draw", 0,
        [&mgr](Marshaler& m) { mgr.impl_ImageList_Draw(m); },
        24
    });

    // Additional ImageList exports resolved via GetProcAddress during DllMain.
    // Stubs return documented defaults until pixel drawing / drag state is
    // wired; presence alone unblocks load.
    auto ignore_args_true = [](u32 n, Marshaler& m) {
        for (u32 i = 0; i < n; ++i) (void)m.arg(i);
        m.set_return(1);
    };
    auto ignore_args_zero = [](u32 n, Marshaler& m) {
        for (u32 i = 0; i < n; ++i) (void)m.arg(i);
        m.set_return(0);
    };

    // ImageList_DrawEx(himl, i, hdc, x, y, dx, dy, rgbBk, rgbFg, fStyle)
    exports.entries.push_back(ExportEntry{
        "ImageList_DrawEx", 0,
        [ignore_args_true](Marshaler& m) { ignore_args_true(10, m); }, 40});
    // ImageList_ReplaceIcon(himl, i, hicon) → index or -1
    exports.entries.push_back(ExportEntry{
        "ImageList_ReplaceIcon", 0,
        [](Marshaler& m) {
            for (u32 i = 0; i < 3; ++i) (void)m.arg(i);
            m.set_return(0); // index 0
        }, 12});
    exports.entries.push_back(ExportEntry{
        "ImageList_Add", 0,
        [&mgr](Marshaler& m) {
            // Reuse AddMasked semantics for count tracking (mask ignored).
            mgr.impl_ImageList_AddMasked(m);
        }, 12});
    exports.entries.push_back(ExportEntry{
        "ImageList_Remove", 0,
        [ignore_args_true](Marshaler& m) { ignore_args_true(2, m); }, 8});
    exports.entries.push_back(ExportEntry{
        "ImageList_SetBkColor", 0,
        [ignore_args_zero](Marshaler& m) { ignore_args_zero(2, m); }, 8});
    exports.entries.push_back(ExportEntry{
        "ImageList_GetBkColor", 0,
        [ignore_args_zero](Marshaler& m) { ignore_args_zero(1, m); }, 4});
    exports.entries.push_back(ExportEntry{
        "ImageList_GetIcon", 0,
        [ignore_args_zero](Marshaler& m) { ignore_args_zero(3, m); }, 12});
    exports.entries.push_back(ExportEntry{
        "ImageList_LoadImageA", 0,
        [ignore_args_zero](Marshaler& m) { ignore_args_zero(7, m); }, 28});
    exports.entries.push_back(ExportEntry{
        "ImageList_LoadImageW", 0,
        [ignore_args_zero](Marshaler& m) { ignore_args_zero(7, m); }, 28});
    exports.entries.push_back(ExportEntry{
        "ImageList_SetImageCount", 0,
        [ignore_args_true](Marshaler& m) { ignore_args_true(2, m); }, 8});
    exports.entries.push_back(ExportEntry{
        "ImageList_GetImageInfo", 0,
        [ignore_args_zero](Marshaler& m) { ignore_args_zero(3, m); }, 12});
    exports.entries.push_back(ExportEntry{
        "ImageList_BeginDrag", 0,
        [ignore_args_true](Marshaler& m) { ignore_args_true(4, m); }, 16});
    exports.entries.push_back(ExportEntry{
        "ImageList_EndDrag", 0,
        [ignore_args_true](Marshaler& m) { ignore_args_true(0, m); }, 0});
    exports.entries.push_back(ExportEntry{
        "ImageList_DragEnter", 0,
        [ignore_args_true](Marshaler& m) { ignore_args_true(3, m); }, 12});
    exports.entries.push_back(ExportEntry{
        "ImageList_DragLeave", 0,
        [ignore_args_true](Marshaler& m) { ignore_args_true(1, m); }, 4});
    exports.entries.push_back(ExportEntry{
        "ImageList_DragMove", 0,
        [ignore_args_true](Marshaler& m) { ignore_args_true(2, m); }, 8});
    exports.entries.push_back(ExportEntry{
        "ImageList_DragShowNolock", 0,
        [ignore_args_true](Marshaler& m) { ignore_args_true(1, m); }, 4});
    exports.entries.push_back(ExportEntry{
        "ImageList_SetOverlayImage", 0,
        [ignore_args_true](Marshaler& m) { ignore_args_true(3, m); }, 12});
    exports.entries.push_back(ExportEntry{
        "ImageList_Duplicate", 0,
        [ignore_args_zero](Marshaler& m) { ignore_args_zero(1, m); }, 4});

    // DllGetVersion (1 arg: DLLVERSIONINFO*, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "DllGetVersion", 0,
        [&mgr](Marshaler& m) { mgr.impl_DllGetVersion(m); },
        4
    });

    // No legacy flat-scroll state is initialized by this runtime. Learn
    // requires forwarding an uninitialized window to standard SetScrollInfo.
    // Preserve its arguments, return value, error behavior and redraw path.
    // A caller building a partial export set must provide the user32 service.
    // https://learn.microsoft.com/windows/win32/api/commctrl/nf-commctrl-flatsb_setscrollinfo
    if (set_scroll_info) {
        exports.entries.push_back(ExportEntry{
            sdk_creation::FlatSB_SetScrollInfoName, 0, std::move(set_scroll_info), sdk_creation::FlatSB_SetScrollInfoStackBytes});
    }

    // Each flat API explicitly delegates to its standard counterpart for an
    // uninitialized HWND. Keep the Marshaler intact, including LastError.
    if (scroll.get_pos) exports.entries.push_back({sdk_creation::FlatSB_GetScrollPosName, 0,
        std::move(scroll.get_pos), sdk_creation::FlatSB_GetScrollPosStackBytes});
    if (scroll.set_pos) exports.entries.push_back({sdk_creation::FlatSB_SetScrollPosName, 0,
        std::move(scroll.set_pos), sdk_creation::FlatSB_SetScrollPosStackBytes});
    if (scroll.get_info) exports.entries.push_back({sdk_creation::FlatSB_GetScrollInfoName, 0,
        std::move(scroll.get_info), sdk_creation::FlatSB_GetScrollInfoStackBytes});
    if (scroll.get_range) exports.entries.push_back({sdk_creation::FlatSB_GetScrollRangeName, 0,
        std::move(scroll.get_range), sdk_creation::FlatSB_GetScrollRangeStackBytes});
    if (scroll.set_range) exports.entries.push_back({sdk_creation::FlatSB_SetScrollRangeName, 0,
        std::move(scroll.set_range), sdk_creation::FlatSB_SetScrollRangeStackBytes});
    if (scroll.enable) exports.entries.push_back({sdk_creation::FlatSB_EnableScrollBarName, 0,
        std::move(scroll.enable), sdk_creation::FlatSB_EnableScrollBarStackBytes});
    if (scroll.show) exports.entries.push_back({sdk_creation::FlatSB_ShowScrollBarName, 0,
        std::move(scroll.show), sdk_creation::FlatSB_ShowScrollBarStackBytes});
    exports.entries.push_back({sdk_creation::InitializeFlatSBName, 0,
        [](Marshaler& m) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comctl32!InitializeFlatSB: legacy flat-scroll state and rendering\n");
            });
            m.set_return(0u);
        }, sdk_creation::InitializeFlatSBStackBytes});

    // Learn: HRESULT UninitializeFlatSB(HWND). This runtime never initializes
    // flat-scroll state, so the window does not have flat scroll bars. The
    // documented result for that state is S_FALSE. E_FAIL would claim a bar
    // is in use, and S_OK would claim a successful revert.
    // https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-uninitializeflatsb
    exports.entries.push_back({sdk_creation::UninitializeFlatSBName, 0,
        [](Marshaler& m) {
            m.set_return(0x00000001u); // S_FALSE
        }, sdk_creation::UninitializeFlatSBStackBytes});

    // Learn: BOOL FlatSB_GetScrollProp(HWND, UINT, LPINT). GetScrollPropPtr is
    // the same function. WSB_PROP_HSTYLE is nonzero only after InitializeFlatSB
    // succeeds; every other property is likewise unavailable here. Return
    // FALSE, leave the caller output untouched, and preserve LastError.
    // https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-flatsb_getscrollprop
    // https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-flatsb_getscrollpropptr
    const auto get_scroll_prop = [](Marshaler& m) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comctl32!FlatSB_GetScrollProp: legacy flat-scroll properties\n");
        });
        m.set_return(0u);
    };
    exports.entries.push_back({sdk_creation::FlatSB_GetScrollPropName, 0,
        get_scroll_prop, sdk_creation::FlatSB_GetScrollPropStackBytes});
    exports.entries.push_back({sdk_creation::FlatSB_GetScrollPropPtrName, 0,
        get_scroll_prop, sdk_creation::FlatSB_GetScrollPropPtrStackBytes});

    // Learn: BOOL FlatSB_SetScrollProp(HWND, UINT, INT_PTR, BOOL).
    // Legacy flat-scroll state/rendering is not implemented. Failure is FALSE;
    // the contract specifies no last-error value. Do not pretend to apply the
    // property, and retain the exact x86 callee-pop size for all four arguments.
    // https://learn.microsoft.com/windows/win32/api/commctrl/nf-commctrl-flatsb_setscrollprop
    exports.entries.push_back(ExportEntry{
        sdk_creation::FlatSB_SetScrollPropName, 0,
        [](Marshaler& m) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comctl32!FlatSB_SetScrollProp: legacy flat-scroll properties and redraw\n");
            });
            m.set_return(0);
        },
        4 * sizeof(u32)
    });

    // Windows 11 26100.9278 x86 comctl32 export table: ordinal 381.
    // commctrl.h: HRESULT (HINSTANCE, PCWSTR, int, int, HICON*), stdcall 20.
    // Until size-selection is implemented, fail with the proper HRESULT and
    // ABI. The generic missing-export fallback cannot unwind an x86 call.
    exports.entries.push_back(ExportEntry{
        "LoadIconWithScaleDown", 381,
        [](Marshaler& m) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comctl32!LoadIconWithScaleDown: icon size selection\n");
            });
            m.set_return(0x80004001u); // E_NOTIMPL
        },
        20
    });

    // LoadIconMetric is exported by name and ordinal 380 in comctl32 v6.
    exports.entries.push_back(ExportEntry{
        "LoadIconMetric", 380,
        [&mgr](Marshaler& m) { mgr.impl_LoadIconMetric(m); },
        16
    });

        // TaskDialogIndirect — commctrl.h. comctl32.lib imports it by ordinal 345,
    // so it must answer to both the name and that ordinal.
    exports.entries.push_back(ExportEntry{
        "TaskDialogIndirect", 345,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.task_dialog_indirect(
                m, m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3)));
        },
        16
    });

    // TaskDialog (6 args) — comctl32.lib imports it by ordinal 344.
    exports.entries.push_back(ExportEntry{
        "TaskDialog", 344,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.task_dialog(
                m, m.arg_ptr(2), m.arg_ptr(3), m.arg_ptr(4),
                static_cast<u32>(m.arg(5)), m.arg_ptr(6), m.arg_ptr(7)));
        },
        32
    });

exports.entries.push_back(ExportEntry{
        "SetWindowSubclass", 410,
        [&mgr](Marshaler& m) { mgr.impl_SetWindowSubclass(m); },
        16
    });

    exports.entries.push_back(ExportEntry{
        "GetWindowSubclass", 411,
        [&mgr](Marshaler& m) { mgr.impl_GetWindowSubclass(m); },
        16
    });

    exports.entries.push_back(ExportEntry{
        "RemoveWindowSubclass", 412,
        [&mgr](Marshaler& m) { mgr.impl_RemoveWindowSubclass(m); },
        12
    });

    exports.entries.push_back(ExportEntry{
        "DefSubclassProc", 413,
        [&mgr](Marshaler& m) { mgr.impl_DefSubclassProc(m); },
        16
    });

    // -- Property sheets (prsht.h). Vitreo has no property-sheet frame (the
    // PSM_* / PSN_* dialog that hosts pages behind a tab control), so it
    // hands out no HPROPSHEETPAGE: a page handle exists only to be passed to
    // PropertySheet or PSM_ADDPAGE, and a success here would promise a sheet
    // that cannot be shown. Every entry fails with its documented failure
    // value; the last error is left untouched, as Windows leaves it on these
    // failures (propsheet_theme_session_probe: createpage_*_minus1,
    // propertysheet_size0 err=dead). comctl32.lib imports all of them by
    // name, and their ordinals differ between comctl32 v5 and v6 and between
    // x86 and x64, so they are registered by name only. The undecorated
    // CreatePropertySheetPage / PropertySheet exports share the A entry
    // point's RVA in both bitnesses.
    {
        // PROPSHEETPAGE{A,W}_V1_SIZE and PROPSHEETHEADER{A,W}_V1_SIZE: 40 on
        // x86, 72 on x64 (propsheet_theme_session_probe "sizes").
        auto v1_size = [](const Marshaler& m) -> u32 {
            return m.cpu_is_64bit() ? 72u : 40u;
        };
        auto create_page = [v1_size](Marshaler& m, const char* api) {
            const GuestAddr psp = m.arg_ptr(0);
            m.set_return(0);
            if (psp == 0) return;
            const auto size = m.read_u32(psp);
            // Windows refuses a structure smaller than the V1 layout before
            // looking at anything else (createpage_size0 / _v1_minus1).
            if (size.is_err() || size.value() < v1_size(m)) return;
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED comctl32!%s: no property-sheet "
                    "frame; returning NULL\n", api);
            }
        };
        auto property_sheet = [v1_size](Marshaler& m, const char* api) {
            const GuestAddr psh = m.arg_ptr(0);
            m.set_return(~0ull);  // -1: "An error occurred."
            if (psh == 0) return;
            const auto size = m.read_u32(psh);
            if (size.is_err() || size.value() < v1_size(m)) return;
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED comctl32!%s: no property-sheet "
                    "frame (modal or PSH_MODELESS); returning -1\n", api);
            }
        };
        exports.entries.push_back(ExportEntry{"CreatePropertySheetPageW", 0,
            [create_page](Marshaler& m) {
                create_page(m, "CreatePropertySheetPageW");
            }, 4});
        exports.entries.push_back(ExportEntry{"CreatePropertySheetPageA", 0,
            [create_page](Marshaler& m) {
                create_page(m, "CreatePropertySheetPageA");
            }, 4});
        exports.entries.push_back(ExportEntry{"CreatePropertySheetPage", 0,
            [create_page](Marshaler& m) {
                create_page(m, "CreatePropertySheetPageA");
            }, 4});
        // BOOL DestroyPropertySheetPage(HPROPSHEETPAGE): no page handle can
        // exist, so every handle is not a page and the call returns zero.
        // (Windows faults on NULL; Vitreo reports the failure instead.)
        exports.entries.push_back(ExportEntry{"DestroyPropertySheetPage", 0,
            [](Marshaler& m) {
                (void)m.arg_ptr(0);
                m.set_return(0);
            }, 4});
        exports.entries.push_back(ExportEntry{"PropertySheetW", 0,
            [property_sheet](Marshaler& m) {
                property_sheet(m, "PropertySheetW");
            }, 4});
        exports.entries.push_back(ExportEntry{"PropertySheetA", 0,
            [property_sheet](Marshaler& m) {
                property_sheet(m, "PropertySheetA");
            }, 4});
        exports.entries.push_back(ExportEntry{"PropertySheet", 0,
            [property_sheet](Marshaler& m) {
                property_sheet(m, "PropertySheetA");
            }, 4});
    }

    return exports;
}

} // namespace vitreo::winapi
