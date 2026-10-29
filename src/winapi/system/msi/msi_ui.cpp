// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_ui.cpp
/// @brief Windows Installer engine area: UI levels, external UI handlers,
/// MsiEnableLog, the UI preview exports, and message routing (msi_ui.hpp).
/// The dialog model, event engine and window layout are msi_ui_dialog.cpp.

#include "winapi/system/msi/msi_ui.hpp"
#include "winapi/system/msi/msi_remote.hpp"
#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_package.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "core/memory/guest_memory.hpp"
#include "capsule/vfs/vfs.hpp"

#include <algorithm>
#include <atomic>
#include <unordered_map>

namespace vitreo::winapi::msi {

namespace {

// Windows error codes these exports return (winerror.h).
constexpr u32 kErrSuccess = 0;
constexpr u32 kErrInvalidHandle = 6;
constexpr u32 kErrNotEnoughMemory = 14;     ///< second MsiEnableUIPreview (probe "preview second")
constexpr u32 kErrInvalidParameter = 87;
constexpr u32 kErrCallNotImplemented = 120;
constexpr u32 kErrFunctionNotCalled = 1626;  ///< MsiPreviewDialog of an unknown / null dialog (probe)
constexpr u32 kErrFunctionFailed = 1627;     ///< MsiPreviewBillboard of an unknown billboard (probe)

std::string acp_to_utf8(const std::string& acp) {
    if (acp.empty()) return {};
    const int n = StringConverter::multi_byte_to_wide_char(0, 0, acp.data(), static_cast<int>(acp.size()), nullptr, 0);
    std::vector<u16> w(static_cast<usize>(n > 0 ? n : 0));
    if (n > 0) StringConverter::multi_byte_to_wide_char(0, 0, acp.data(), static_cast<int>(acp.size()), w.data(), n);
    return utf16_to_utf8(w.data(), static_cast<int>(w.size()));
}

/// A string argument of an A or W export; nullopt for a null pointer or an
/// unreadable one.
std::optional<std::string> read_string_arg(Marshaler& m, GuestAddr p, bool wide) {
    if (p == 0) return std::nullopt;
    if (wide) {
        auto s = m.read_wide_string(p);
        if (s.is_err()) return std::nullopt;
        return s.value();
    }
    auto s = m.read_ansi_string(p);
    if (s.is_err()) return std::nullopt;
    return acp_to_utf8(s.value());
}

/// The UI preview object behind an MsiEnableUIPreview handle. The preview
/// ends when the handle closes (MsiQuery.h comment).
struct UiPreview;

struct ProcessUi {
    std::mutex mutex;
    UiProcessState state;
    std::shared_ptr<UiGuest> guest;
    std::shared_ptr<WindowSystem> windows;
    /// The open preview (one per process: probe "preview second" -> 14).
    std::weak_ptr<UiPreview> preview;
    /// The process's internal UI (install_internal_ui).
    std::unique_ptr<InstallerUi> host;
    /// The installer service's relay to the client of the running install.
    ClientUiRelay relay;
};

std::mutex g_registry_mutex;
std::unordered_map<MsiManager*, std::shared_ptr<ProcessUi>>& registry() {
    static std::unordered_map<MsiManager*, std::shared_ptr<ProcessUi>> r;
    return r;
}
std::shared_ptr<ProcessUi> process_ui(MsiManager& mgr) {
    std::lock_guard lock(g_registry_mutex);
    auto& slot = registry()[&mgr];
    if (!slot) slot = std::make_shared<ProcessUi>();
    return slot;
}

thread_local int t_custom_action_depth = 0;

std::u16string to_u16(const std::string& utf8) {
    const std::vector<u16> w = utf8_to_utf16(utf8);
    std::u16string out(w.size(), u'\0');
    for (usize i = 0; i < w.size(); ++i) out[i] = static_cast<char16_t>(w[i]);
    while (!out.empty() && out.back() == u'\0') out.pop_back();
    return out;
}

// ---- The guest-call UiGuest ------------------------------------------------------

class GuestCallUi final : public UiGuest {
public:
    GuestCallUi(GuestMemory& memory, UiGuestCall call) : m_memory(memory), m_call(std::move(call)) {}

    i32 call_string_handler(const ExternalStringHandler& h, u32 type, const std::u16string* text) override {
        GuestAddr str = 0;
        if (text != nullptr) {
            std::vector<u8> bytes;
            if (h.unicode) {
                bytes.resize((text->size() + 1) * 2, 0);
                for (usize i = 0; i < text->size(); ++i) {
                    bytes[i * 2] = static_cast<u8>((*text)[i] & 0xFF);
                    bytes[i * 2 + 1] = static_cast<u8>((*text)[i] >> 8);
                }
            } else {
                std::vector<u16> w(text->begin(), text->end());
                const std::string acp = utf8_to_acp(utf16_to_utf8(w.data(), static_cast<int>(w.size())));
                bytes.assign(acp.begin(), acp.end());
                bytes.push_back(0);
            }
            str = scratch(bytes.size());
            if (str == 0) {
                log_unimplemented("ExternalUI", "no guest scratch for the handler string");
                return 0;
            }
            (void)m_memory.write_bytes(str, reinterpret_cast<const Byte*>(bytes.data()), bytes.size());
        }
        const u64 args[3] = {h.context, type, str};
        return static_cast<i32>(static_cast<u32>(m_call(h.function, args, 3, h.guest_64bit)));
    }

    i32 call_record_handler(const ExternalRecordHandler& h, u32 type, u32 record) override {
        const u64 args[3] = {h.context, type, record};
        return static_cast<i32>(static_cast<u32>(m_call(h.function, args, 3, h.guest_64bit)));
    }

private:
    /// A per-thread guest page for handler strings (a handler may run on any
    /// thread that drives an install). Grows for long messages.
    GuestAddr scratch(usize bytes) {
        thread_local GuestAddr t_base = 0;
        thread_local usize t_size = 0;
        thread_local const GuestMemory* t_memory = nullptr;
        if (t_base != 0 && t_memory == &m_memory && t_size >= bytes && m_memory.is_mapped(t_base, 2)) return t_base;
        const usize size = (bytes + 0xFFF) & ~static_cast<usize>(0xFFF);
        static std::atomic<u32> s_slot{0};
        const GuestAddr prefer = 0x5A400000 + static_cast<GuestAddr>(s_slot.fetch_add(1) % 64) * 0x40000;
        GuestAddr base = m_memory.find_free_gap(prefer, size);
        if (base == 0) base = m_memory.find_free_gap(0x10000000, size);
        if (base == 0 || m_memory.map_region(base, size, MemoryProtection::ReadWrite).is_err()) return 0;
        t_base = base;
        t_size = size;
        t_memory = &m_memory;
        return base;
    }

    GuestMemory& m_memory;
    UiGuestCall m_call;
};

// ---- UI preview -------------------------------------------------------------------

struct UiPreview {
    std::shared_ptr<Package> package;
    std::unique_ptr<Session> session;
    std::unique_ptr<InstallerUi> ui;
    std::shared_ptr<ProcessUi> owner;
};

}  // namespace

// ---- Process state API --------------------------------------------------------------

UiProcessState ui_process_state(MsiManager& mgr) {
    auto p = process_ui(mgr);
    std::lock_guard lock(p->mutex);
    return p->state;
}

void reset_ui_process_state(MsiManager& mgr) {
    std::shared_ptr<ProcessUi> gone;
    {
        std::lock_guard lock(g_registry_mutex);
        auto it = registry().find(&mgr);
        if (it != registry().end()) {
            gone = it->second;
            registry().erase(it);
        }
    }
    if (gone && gone->host) mgr.set_ui_host(nullptr);
    mgr.set_engine_ui_level(sdk::INSTALLUILEVEL_DEFAULT);
}

u32 ui_internal_level(MsiManager& mgr) { return ui_process_state(mgr).level; }

void set_ui_guest(MsiManager& mgr, std::shared_ptr<UiGuest> guest) {
    auto p = process_ui(mgr);
    std::lock_guard lock(p->mutex);
    p->guest = std::move(guest);
}

void set_ui_window_system(MsiManager& mgr, std::shared_ptr<WindowSystem> windows) {
    auto p = process_ui(mgr);
    std::lock_guard lock(p->mutex);
    p->windows = std::move(windows);
}

std::shared_ptr<WindowSystem> ui_window_system(MsiManager& mgr) {
    auto p = process_ui(mgr);
    std::lock_guard lock(p->mutex);
    return p->windows;
}

UiHooks default_ui_hooks(MsiManager& mgr) {
    UiHooks h;
    capsule::Vfs* vfs = mgr.vfs();
    if (vfs == nullptr) return h;
    h.list_folders = [vfs](Session&, const std::string& path) {
        std::vector<std::string> out;
        auto list = vfs->list_dir(path);
        if (list.is_err()) return out;
        for (const auto& e : list.value()) {
            if (e.name == "." || e.name == "..") continue;
            if ((e.info.attributes & capsule::FileAttributes::Directory) != capsule::FileAttributes::None) out.push_back(e.name);
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    h.create_folder = [vfs](Session&, const std::string& path) { return vfs->create_dir(path).is_ok(); };
    h.list_volumes = [vfs](Session&) {
        std::vector<std::string> out;
        for (char c : vfs->mounted_drives()) out.push_back(std::string(1, c) + ":\\");
        return out;
    };
    h.check_path = [vfs](Session&, const std::string& path, bool writable) {
        // A full path ("X:\..." on a mounted drive, or UNC). For a writable
        // path the deepest existing ancestor must be a folder.
        if (path.size() < 3 || path[1] != ':' || (path[2] != '\\' && path[2] != '/')) return path.rfind("\\\\", 0) == 0;
        if (!vfs->is_drive_mounted(path[0])) return false;
        for (usize i = 2; i < path.size(); ++i) {
            const char c = path[i];
            if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*' || c == ':') return false;
        }
        if (!writable) return true;
        std::string p = path;
        while (p.size() > 3) {
            auto st = vfs->stat(p);
            if (st.is_ok()) return (st.value().attributes & capsule::FileAttributes::Directory) != capsule::FileAttributes::None;
            while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
            const usize slash = p.find_last_of("\\/");
            if (slash == std::string::npos) break;
            p.resize(slash + 1);
        }
        return true;
    };
    return h;
}

void install_internal_ui(MsiManager& mgr, std::shared_ptr<WindowSystem> windows) {
    auto p = process_ui(mgr);
    std::unique_ptr<InstallerUi> old;
    {
        std::lock_guard lock(p->mutex);
        p->windows = windows;
        old = std::move(p->host);
        if (windows) p->host = std::make_unique<InstallerUi>(mgr, windows, default_ui_hooks(mgr), 0);
        mgr.set_ui_host(p->host.get());
    }
}

std::shared_ptr<UiGuest> make_guest_call_ui(GuestMemory& memory, UiGuestCall call) {
    return std::make_shared<GuestCallUi>(memory, std::move(call));
}

CustomActionUiScope::CustomActionUiScope() { ++t_custom_action_depth; }
CustomActionUiScope::~CustomActionUiScope() { --t_custom_action_depth; }
bool CustomActionUiScope::active() { return t_custom_action_depth > 0 || custom_action_server_process(); }

InternalUiChange apply_internal_ui_level(u32 stored, u32 requested) {
    InternalUiChange c;
    const u32 modifiers = requested & kUiLevelModifiers;
    const u32 base_request = requested & ~kUiLevelModifiers;
    const u32 stored_base = stored & ~kUiLevelModifiers;
    if (base_request > sdk::INSTALLUILEVEL_FULL) {
        // Probe: 6, 0x405, 0x80000005, 0x3e8 -> NOCHANGE returned, base kept,
        // modifier bits still taken from the request.
        c.valid = false;
        c.returned = sdk::INSTALLUILEVEL_NOCHANGE;
        c.new_level = stored_base | modifiers;
        return c;
    }
    c.returned = stored;
    c.new_level = (base_request == sdk::INSTALLUILEVEL_NOCHANGE ? stored_base : base_request) | modifiers;
    return c;
}

// ---- Message routing -------------------------------------------------------------------

namespace {

std::u16string record_text_u16(const Record* record, const RecordFormatter& format) {
    if (record == nullptr) return {};
    std::string text = format ? format(*record) : Session::format_record_without_session(*record);
    return to_u16(text);
}

}  // namespace

i32 dispatch_external_ui(MsiManager& mgr, u32 type, const Record* record, const RecordFormatter& format) {
    auto p = process_ui(mgr);
    ExternalRecordHandler rh;
    ExternalStringHandler sh;
    std::shared_ptr<UiGuest> guest;
    {
        std::lock_guard lock(p->mutex);
        rh = p->state.record_handler;
        sh = p->state.string_handler;
        guest = p->guest;
    }
    const u32 bit = log_mode_bit(type);
    const bool want_record = rh.function != 0 && (rh.filter & bit) != 0;
    const bool want_string = sh.function != 0 && (sh.filter & bit) != 0;
    if (!want_record && !want_string) return 0;
    if (!guest) {
        // A registered handler with no way into the guest: the message is
        // not handled (0), which leaves it to the internal UI as Windows
        // does for a handler that declines it.
        log_unimplemented("ExternalUI", "handler registered but the process has no guest-call bridge");
        return 0;
    }
    if (want_record) {
        // The handle lives for the duration of the call (Learn: "Monitoring
        // an Installation Using MsiSetExternalUIRecord": do not close it).
        u32 handle = 0;
        if (record != nullptr) handle = mgr.handles().add(std::make_shared<Record>(*record));
        const i32 r = guest->call_record_handler(rh, type, handle);
        if (handle != 0) mgr.handles().close(handle);
        if (r != 0) return r;
    }
    if (want_string) {
        if (record == nullptr) return guest->call_string_handler(sh, type, nullptr);
        const std::u16string text = record_text_u16(record, format);
        return guest->call_string_handler(sh, type, &text);
    }
    return 0;
}

void set_client_ui_relay(MsiManager& mgr, ClientUiRelay relay) {
    auto p = process_ui(mgr);
    std::lock_guard lock(p->mutex);
    p->relay = std::move(relay);
}

i32 route_message(MsiManager& mgr, UiHost* internal, Session* session, u32 type, const Record* record,
                  const RecordFormatter& format) {
    ClientUiRelay relay;
    {
        auto p = process_ui(mgr);
        std::lock_guard lock(p->mutex);
        relay = p->relay;
    }
    if (relay) {
        // The service's engine: the client owns the handlers and the UI.
        std::string text;
        if (record != nullptr) text = format ? format(*record) : Session::format_record_without_session(*record);
        return relay(type, record, text);
    }
    const i32 external = dispatch_external_ui(mgr, type, record, format);
    if (external != 0) return external;
    if (internal == nullptr || session == nullptr) return 0;
    const Record empty(0);
    return internal->message(*session, type, record != nullptr ? *record : empty);
}

// ---- Exports ----------------------------------------------------------------------------

namespace {

void set_internal_ui(MsiManager& mgr, Marshaler& m) {
    // INSTALLUILEVEL MsiSetInternalUI(INSTALLUILEVEL dwUILevel, HWND* phWnd)
    const u32 requested = m.arg32(0);
    const GuestAddr hwnd_ptr = m.arg_ptr(1);
    auto p = process_ui(mgr);
    std::lock_guard lock(p->mutex);
    const InternalUiChange c = apply_internal_ui_level(p->state.level, requested);
    p->state.level = c.new_level;
    // The engine starts MsiInstallProduct / MsiOpenPackage sessions at this level.
    mgr.set_engine_ui_level(c.new_level);
    // Probe "owner invalid": an invalid level leaves the owner and *phWnd alone.
    if (c.valid && hwnd_ptr != 0) {
        auto requested_owner = m.read_ptr(hwnd_ptr);
        if (requested_owner.is_ok()) {
            (void)m.write_ptr(hwnd_ptr, p->state.owner);
            p->state.owner = requested_owner.value();
        }
    }
    m.set_return(c.returned);
}

void set_external_ui(MsiManager& mgr, Marshaler& m, bool unicode) {
    // INSTALLUI_HANDLER MsiSetExternalUI(INSTALLUI_HANDLER, DWORD dwMessageFilter, LPVOID pvContext)
    const GuestAddr handler = m.arg_ptr(0);
    const u32 filter = m.arg32(1);
    const GuestAddr context = m.arg_ptr(2);
    auto p = process_ui(mgr);
    std::lock_guard lock(p->mutex);
    ExternalStringHandler& slot = p->state.string_handler;
    // One slot for both character sets; the previous handler is returned
    // only to a caller of the same set (probe extW1..extW6).
    const GuestAddr previous = (slot.unicode == unicode) ? slot.function : 0;
    slot.function = handler;
    slot.unicode = unicode;
    slot.filter = filter;
    slot.context = context;
    slot.guest_64bit = m.cpu_is_64bit();
    m.set_return(previous);
}

void set_external_ui_record(MsiManager& mgr, Marshaler& m) {
    // UINT MsiSetExternalUIRecord(INSTALLUI_HANDLER_RECORD, DWORD, LPVOID, PINSTALLUI_HANDLER_RECORD)
    if (CustomActionUiScope::active()) {
        m.set_return(kErrCallNotImplemented);
        return;
    }
    const GuestAddr handler = m.arg_ptr(0);
    const u32 filter = m.arg32(1);
    const GuestAddr context = m.arg_ptr(2);
    const GuestAddr prev_out = m.arg_ptr(3);
    auto p = process_ui(mgr);
    std::lock_guard lock(p->mutex);
    ExternalRecordHandler& slot = p->state.record_handler;
    if (prev_out != 0) (void)m.write_ptr(prev_out, slot.function);
    slot.function = handler;
    slot.filter = filter;
    slot.context = context;
    slot.guest_64bit = m.cpu_is_64bit();
    m.set_return(kErrSuccess);
}

void enable_log(MsiManager& mgr, Marshaler& m, bool wide) {
    // UINT MsiEnableLog(DWORD dwLogMode, LPCTSTR szLogFile, DWORD dwLogAttributes)
    const u32 mode = m.arg32(0);
    const GuestAddr file_ptr = m.arg_ptr(1);
    const u32 attributes = m.arg32(2);
    auto p = process_ui(mgr);
    std::lock_guard lock(p->mutex);
    if (file_ptr == 0) {
        // "Entering a null disables logging, in which case dwLogMode is ignored."
        p->state.log = LogRequest{};
        m.set_return(kErrSuccess);
        return;
    }
    if (mode == 0) {
        // "If a path is supplied, then dwLogMode must not be zero" (probe: 87).
        m.set_return(kErrInvalidParameter);
        return;
    }
    const auto path = read_string_arg(m, file_ptr, wide);
    if (!path) {
        m.set_return(kErrInvalidParameter);
        return;
    }
    // Probe: unknown mode / attribute bits, a missing directory and "" are
    // all accepted, and nothing is created until an install logs.
    p->state.log.enabled = true;
    p->state.log.mode = mode;
    p->state.log.path = *path;
    p->state.log.attributes = attributes;
    m.set_return(kErrSuccess);
}

std::shared_ptr<UiPreview> find_preview(MsiManager& mgr, u32 handle) { return mgr.handles().get<UiPreview>(handle); }

void enable_ui_preview(MsiManager& mgr, Marshaler& m) {
    // UINT MsiEnableUIPreview(MSIHANDLE hDatabase, MSIHANDLE* phPreview)
    const u32 hdb = m.arg32(0);
    const GuestAddr out = m.arg_ptr(1);
    std::shared_ptr<Package> package = mgr.engine_database_package(hdb);
    if (!package) {
        m.set_return(kErrInvalidHandle);
        return;
    }
    if (out == 0) {
        m.set_return(kErrInvalidParameter);
        return;
    }
    auto p = process_ui(mgr);
    std::shared_ptr<WindowSystem> windows;
    {
        std::lock_guard lock(p->mutex);
        if (!p->preview.expired()) {
            m.set_return(kErrNotEnoughMemory);
            return;
        }
        windows = p->windows;
    }
    auto preview = std::make_shared<UiPreview>();
    preview->package = package;
    preview->session = std::make_unique<Session>();
    if (!preview->session->initialize(package)) {
        // A database with no Property table still previews (its dialogs
        // format without properties).
        preview->session = std::make_unique<Session>();
    }
    preview->owner = p;
    if (windows) preview->ui = std::make_unique<InstallerUi>(mgr, windows, UiHooks{}, sdk::INSTALLUILEVEL_FULL);
    {
        std::lock_guard lock(p->mutex);
        p->preview = preview;
    }
    const u32 h = mgr.handles().add(preview);
    (void)m.write_u32(out, h);
    m.set_return(kErrSuccess);
}

}  // namespace

/// Preview hooks into the dialog engine (msi_ui_dialog.cpp).
u32 preview_show_dialog(InstallerUi& ui, Session& session, const std::string& dialog);
u32 preview_show_billboard(InstallerUi& ui, Session& session, const std::string& control, const std::string* billboard);

namespace {

void preview_dialog(MsiManager& mgr, Marshaler& m, bool wide) {
    // UINT MsiPreviewDialog(MSIHANDLE hPreview, LPCTSTR szDialogName)
    auto preview = find_preview(mgr, m.arg32(0));
    if (!preview) {
        m.set_return(kErrInvalidHandle);
        return;
    }
    const auto name = read_string_arg(m, m.arg_ptr(1), wide);
    if (!name) {
        m.set_return(kErrFunctionNotCalled);  // probe "previewdlg null"
        return;
    }
    if (!name->empty() && !load_dialog(*preview->package, *name)) {
        m.set_return(kErrFunctionNotCalled);  // probe "previewdlg unknown"
        return;
    }
    if (!preview->ui) {
        log_unimplemented(wide ? "MsiPreviewDialogW" : "MsiPreviewDialogA", "no window system in this process");
        m.set_return(name->empty() ? kErrSuccess : kErrFunctionFailed);
        return;
    }
    m.set_return(preview_show_dialog(*preview->ui, *preview->session, *name));
}

void preview_billboard(MsiManager& mgr, Marshaler& m, bool wide) {
    // UINT MsiPreviewBillboard(MSIHANDLE hPreview, LPCTSTR szControlName, LPCTSTR szBillboard)
    auto preview = find_preview(mgr, m.arg32(0));
    if (!preview) {
        m.set_return(kErrInvalidHandle);
        return;
    }
    const auto control = read_string_arg(m, m.arg_ptr(1), wide);
    if (!control) {
        m.set_return(kErrInvalidParameter);  // probe "previewbb nullctl"
        return;
    }
    const auto billboard = read_string_arg(m, m.arg_ptr(2), wide);
    if (billboard && load_billboard_controls(*preview->package, *billboard).empty()) {
        m.set_return(kErrFunctionFailed);  // probe "previewbb unknownbb"
        return;
    }
    if (!preview->ui) {
        if (!billboard) {
            m.set_return(kErrSuccess);  // nothing to remove
            return;
        }
        log_unimplemented(wide ? "MsiPreviewBillboardW" : "MsiPreviewBillboardA", "no window system in this process");
        m.set_return(kErrFunctionFailed);
        return;
    }
    m.set_return(preview_show_billboard(*preview->ui, *preview->session, *control,
                                        billboard ? &*billboard : nullptr));
}

}  // namespace

void register_ui_api(MsiManager& mgr, HandlerMap& map) {
    map["MsiSetInternalUI"] = [&mgr](Marshaler& m) { set_internal_ui(mgr, m); };
    map["MsiSetExternalUIA"] = [&mgr](Marshaler& m) { set_external_ui(mgr, m, false); };
    map["MsiSetExternalUIW"] = [&mgr](Marshaler& m) { set_external_ui(mgr, m, true); };
    map["MsiSetExternalUIRecord"] = [&mgr](Marshaler& m) { set_external_ui_record(mgr, m); };
    map["MsiEnableLogA"] = [&mgr](Marshaler& m) { enable_log(mgr, m, false); };
    map["MsiEnableLogW"] = [&mgr](Marshaler& m) { enable_log(mgr, m, true); };
    map["MsiEnableUIPreview"] = [&mgr](Marshaler& m) { enable_ui_preview(mgr, m); };
    map["MsiPreviewDialogA"] = [&mgr](Marshaler& m) { preview_dialog(mgr, m, false); };
    map["MsiPreviewDialogW"] = [&mgr](Marshaler& m) { preview_dialog(mgr, m, true); };
    map["MsiPreviewBillboardA"] = [&mgr](Marshaler& m) { preview_billboard(mgr, m, false); };
    map["MsiPreviewBillboardW"] = [&mgr](Marshaler& m) { preview_billboard(mgr, m, true); };
}

}  // namespace vitreo::winapi::msi
