// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_winrt_statics.cpp
/// @brief Toast, KeyCredential, UIViewSettings, PenDevice and AppCapability statics

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// Remaining WinRT runtime classes.
//
// Each factory activates, so a guest's cppwinrt factory cache stops throwing
// hresult_class_not_registered; the methods behind them answer truthfully
// where this host can, and refuse with the documented HRESULT where the
// facility genuinely is not there.
// ---------------------------------------------------------------------------

// -- Windows.UI.Notifications.ToastNotificationManager ----------------------
void CombaseManager::impl_ToastCreateNotifier(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    VITREO_TRACE(
        "[combase] ToastNotificationManager.CreateToastNotifier refused: no "
        "host notification sink is wired up\n");
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_ToastCreateNotifierWithId(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_ToastGetTemplateContent(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    // The template is an XmlDocument, a runtime class of its own.
    m.set_return(E_NOTIMPL);
}

// -- Windows.Security.Credentials.KeyCredentialManager ----------------------
void CombaseManager::impl_KeyCredIsSupportedAsync(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    // A truthful answer: this machine has no Windows Hello credential store,
    // so the operation completes with false rather than failing. That is the
    // path a caller is written to handle.
    const GuestAddr op = create_completed_async_scalar(0, 1, m.cpu_is_64bit());
    if (op == 0) { m.set_return(E_OUTOFMEMORY); return; }
    m.set_return(m.write_ptr(out, op).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_KeyCredRenewAttestationAsync(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_KeyCredRequestCreateAsync(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_KeyCredOpenAsync(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_KeyCredDeleteAsync(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    m.set_return(E_NOTIMPL);
}

// -- Windows.UI.ViewManagement.UIViewSettings -------------------------------
void CombaseManager::impl_UIViewSettingsGetForWindow(Marshaler& m) {
    // IUIViewSettingsInterop::GetForWindow(HWND, REFIID, void**).
    const GuestAddr window = m.arg_ptr(1);
    const GuestAddr iid_ptr = m.arg_ptr(2);
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    if (window == 0) { m.set_return(E_INVALIDARG); return; }
    if (iid_ptr == 0) { m.set_return(E_POINTER); return; }
    CombaseGuid iid{};
    if (m_memory.read_bytes(
            iid_ptr, reinterpret_cast<Byte*>(&iid), sizeof(iid)).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    if (!guid_equal(iid, IID_IUI_VIEW_SETTINGS)) {
        m.set_return(E_NOINTERFACE);
        return;
    }
    Object view{};
    view.type = ObjectType::UIViewSettings;
    view.ref_count = 1;
    const GuestAddr object = allocate_object(std::move(view), m.cpu_is_64bit());
    if (object == 0) { m.set_return(E_OUTOFMEMORY); return; }
    m.set_return(m.write_ptr(out, object).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_UIViewSettingsGetUserInteractionMode(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    // UserInteractionMode::Mouse = 0. Vitreo presents a desktop with a
    // pointer, never a tablet posture, so this is a real answer.
    m.set_return(m.write_u32(out, 0u).is_ok() ? S_OK : E_POINTER);
}

// -- Windows.Devices.Input.PenDevice ----------------------------------------
void CombaseManager::impl_PenDeviceGetFromPointerId(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) { m.set_return(E_POINTER); return; }
    // No digitizer: a pointer id never belongs to a pen, and null is the
    // documented result for that, not an error.
    m.set_return(m.write_ptr(out, 0).is_ok() ? S_OK : E_POINTER);
}

// -- Windows.Security.Authorization.AppCapabilityAccess.AppCapability -------
void CombaseManager::impl_AppCapabilityRequestAccessForCapabilitiesAsync(
    Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_AppCapabilityRequestAccessForCapabilitiesForUserAsync(
    Marshaler& m) {
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_AppCapabilityCreate(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    // Capability access is a packaged-app concept; an unpackaged guest has no
    // capability to name.
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_AppCapabilityCreateForUser(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(4);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    m.set_return(E_NOTIMPL);
}

} // namespace vitreo::winapi
