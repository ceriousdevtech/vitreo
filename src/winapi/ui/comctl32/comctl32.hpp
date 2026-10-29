#pragma once
/// @file comctl32.hpp
/// @brief COMCTL32 common controls stubs

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

class Marshaler;
class ResourceManager;
}

namespace vitreo::windowing {
class HwndManager;
using HWND = u32;
}

namespace vitreo::winapi {

/// State for a single image list instance
struct ImageListState {
    i32 cx;
    i32 cy;
    u32 flags;
    i32 initial;
    i32 grow;
    i32 count;
};

/// Stub manager for COMCTL32 common controls APIs
class ComCtl32Manager {
public:
    using GuestCall = std::function<u64(
        GuestAddr, const u64*, usize, bool)>;

    ComCtl32Manager() = default;

    void set_resource_manager(ResourceManager* resource_mgr) {
        m_resource_mgr = resource_mgr;
    }
    void set_hwnd_manager(windowing::HwndManager* hwnd_mgr) {
        m_hwnd_mgr = hwnd_mgr;
    }

    /// TaskDialogIndirect — commctrl.h. `config` points at a TASKDIALOGCONFIG
    /// in guest memory; the three optional out-parameters may each be null.
    /// @return HRESULT (S_OK, or E_INVALIDARG for a malformed config)
    [[nodiscard]] u32 task_dialog_indirect(Marshaler& m, GuestAddr config,
                                           GuestAddr out_button,
                                           GuestAddr out_radio,
                                           GuestAddr out_verification);

    /// TaskDialog — the simple form, which commctrl.h documents as equivalent
    /// to TaskDialogIndirect with only the common buttons.
    [[nodiscard]] u32 task_dialog(Marshaler& m, GuestAddr title,
                                  GuestAddr instruction, GuestAddr content,
                                  u32 common_buttons, GuestAddr icon,
                                  GuestAddr out_button);
    void set_guest_call(GuestCall call) {
        m_guest_call = std::move(call);
    }

    /// Create a tracked image list for another WinAPI subsystem.
    [[nodiscard]] u32 create_image_list(i32 cx, i32 cy, u32 flags,
                                        i32 initial, i32 grow);

    /// InitCommonControls — no-op (ordinal #17)
    void impl_InitCommonControls(Marshaler& m) const;

    /// ImageList_Create — creates tracked image list, returns handle
    void impl_ImageList_Create(Marshaler& m);

    /// ImageList_AddMasked — increments image count, returns index
    void impl_ImageList_AddMasked(Marshaler& m);

    /// ImageList_Destroy — removes from state map
    void impl_ImageList_Destroy(Marshaler& m);

    /// InitCommonControlsEx — logs ICC flags, returns TRUE
    void impl_InitCommonControlsEx(Marshaler& m);

    /// The icon size of a live image list (ImageList_GetIconSize's answer).
    [[nodiscard]] bool image_list_size(u64 himl, i32& cx, i32& cy) const {
        const auto it = m_image_lists.find(static_cast<u32>(himl));
        if (it == m_image_lists.end()) return false;
        cx = it->second.cx;
        cy = it->second.cy;
        return true;
    }
    /// ImageList_GetIconSize — writes cx/cy to output pointers
    void impl_ImageList_GetIconSize(Marshaler& m);

    /// ImageList_SetIconSize — sets cx/cy and clears all images (Learn commctrl)
    void impl_ImageList_SetIconSize(Marshaler& m);

    /// ImageList_GetImageCount — returns tracked image count (Learn commctrl)
    void impl_ImageList_GetImageCount(Marshaler& m);

    /// ImageList_Draw — success for known lists (pixels not rendered yet)
    void impl_ImageList_Draw(Marshaler& m);

    /// DllGetVersion — fills DLLVERSIONINFO and returns S_OK
    void impl_DllGetVersion(Marshaler& m);

    /// LoadIconMetric — loads a module/system icon at a standard metric.
    void impl_LoadIconMetric(Marshaler& m);

    /// Install or update a window subclass callback.
    [[nodiscard]] bool set_window_subclass(
        windowing::HWND hwnd, GuestAddr proc, u64 id, u64 ref_data);

    /// Remove a window subclass callback identified by (proc, id).
    [[nodiscard]] bool remove_window_subclass(
        windowing::HWND hwnd, GuestAddr proc, u64 id);

    /// Retrieve the reference data for a window subclass callback.
    [[nodiscard]] bool get_window_subclass(
        windowing::HWND hwnd, GuestAddr proc, u64 id, u64& ref_data) const;

    /// Dispatch through the comctl32 subclass chain and then the original
    /// window procedure. This is the shared path used by DispatchMessage,
    /// SendMessage, and synchronous creation/destruction messages.
    [[nodiscard]] u64 dispatch_window_message(
        windowing::HWND hwnd, u32 msg, u64 w_param, u64 l_param,
        GuestAddr original_wndproc, bool is_64bit);

    void impl_SetWindowSubclass(Marshaler& m);
    void impl_GetWindowSubclass(Marshaler& m);
    void impl_RemoveWindowSubclass(Marshaler& m);
    void impl_DefSubclassProc(Marshaler& m);

private:
    struct SubclassEntry {
        GuestAddr proc = 0;
        u64 id = 0;
        u64 ref_data = 0;
    };

    struct SubclassKey {
        GuestAddr proc = 0;
        u64 id = 0;
    };

    struct DispatchFrame {
        ComCtl32Manager* manager = nullptr;
        windowing::HWND hwnd = 0;
        GuestAddr original_wndproc = 0;
        bool is_64bit = false;
        std::vector<SubclassKey> order;
        usize next = 0;
    };

    [[nodiscard]] bool is_calling_thread_owner(windowing::HWND hwnd) const;
    [[nodiscard]] bool find_subclass(
        windowing::HWND hwnd, const SubclassKey& key,
        SubclassEntry& entry) const;
    [[nodiscard]] u64 invoke_next(
        DispatchFrame& frame, windowing::HWND hwnd, u32 msg,
        u64 w_param, u64 l_param);

    static thread_local std::vector<DispatchFrame*> s_dispatch_stack;

    ResourceManager* m_resource_mgr = nullptr;
    windowing::HwndManager* m_hwnd_mgr = nullptr;
    GuestCall m_guest_call;
    mutable std::mutex m_subclass_mutex;
    std::unordered_map<windowing::HWND, std::vector<SubclassEntry>>
        m_subclasses;
    std::unordered_map<u32, ImageListState> m_image_lists;
    u32 m_next_imglist_handle{0xABCD0001};
};

// Standard USER32 services used when no legacy flat-scroll state exists.
struct FlatScrollHandlers {
    ApiHandler get_pos, set_pos, get_info, get_range, set_range, enable, show;
};

DllExports create_comctl32_thunks(ComCtl32Manager& mgr, ApiHandler track_mouse = {},
                                    ApiHandler set_scroll_info = {},
                                    FlatScrollHandlers scroll = {});

} // namespace vitreo::winapi
