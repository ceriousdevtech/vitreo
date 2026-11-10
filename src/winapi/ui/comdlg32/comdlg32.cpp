#include "comdlg32.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "capsule/vfs/vfs.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace vitreo::winapi {
namespace {
thread_local u32 dialog_error = 0;

// Count UTF-16 code units rather than UTF-8 bytes for W buffer limits/offsets.
size_t string_units(const std::string& text, bool wide) {
    if (!wide) return text.size();
    size_t units = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) units += c >= 0xF0 ? 2 : 1;
    }
    return units;
}

std::optional<std::string> guest_dialog_path(capsule::Vfs* vfs, const std::string& host) {
    if (!vfs) return std::nullopt;
    const auto path = std::filesystem::path(host).lexically_normal();
    std::string best;
    size_t best_root = 0;
    for (char drive : vfs->mounted_drives()) {
        const std::string prefix = std::string(1, drive) + ":\\";
        auto root = vfs->resolve_host_path(prefix);
        if (root.is_err()) continue;
        const auto root_path = std::filesystem::path(root.value()).lexically_normal();
        const auto relative = path.lexically_relative(root_path);
        if (relative.empty() || (!relative.empty() && *relative.begin() == "..")) continue;
        if (root_path.string().size() < best_root) continue;
        best_root = root_path.string().size();
        best = prefix + (relative == "." ? "" : relative.generic_string());
        std::replace(best.begin(), best.end(), '/', '\\');
    }
    if (best.empty()) return std::nullopt;
    return best;
}
}

std::optional<std::string> guest_path_for_host(capsule::Vfs* vfs, const std::string& host) {
    return guest_dialog_path(vfs, host);
}

DllExports create_comdlg32_thunks(windowing::CocoaBridge* bridge, capsule::Vfs* vfs,
                                  FileDialogPresenter presenter,
                                  ColorDialogPresenter color_presenter) {
    DllExports exports;
    auto file_dialog = [bridge, vfs, presenter](Marshaler& m, bool save, bool wide) {
        dialog_error = 0;
        const GuestAddr ofn = m.arg_ptr(0);
        if (!ofn || (!bridge && !presenter)) { m.set_return(0u); return; }
        const bool x64 = m.cpu_is_64bit();
        const auto pointer = [&](u32 off32, u32 off64) {
            return m.read_ptr(ofn + (x64 ? off64 : off32)).value_or(0);
        };
        const auto read_string = [&](GuestAddr ptr) {
            if (!ptr) return std::string{};
            auto r = wide ? m.read_wide_string(ptr) : m.read_ansi_string(ptr);
            return r.is_ok() ? r.value() : std::string{};
        };
        const GuestAddr file = pointer(28, 48);
        const u32 capacity = m.read_u32(ofn + (x64 ? 56 : 32)).value_or(0);
        if (!file || !capacity) { dialog_error = 2; m.set_return(0u); return; }
        FileDialogRequest request{save, read_string(pointer(44, 80)),
            read_string(file), {}, read_string(pointer(48, 88))};
        // Filter entries are description/pattern pairs separated by NULs.
        GuestAddr filter = pointer(12, 24);
        const u32 index_offset = x64 ? 44 : 24;
        u32 index = std::max(1u, m.read_u32(ofn + index_offset).value_or(1));
        for (u32 i = 1; filter && i <= index && i <= 256; ++i) {
            const auto label = read_string(filter);
            if (label.empty()) break;
            filter += (string_units(label, wide) + 1) * (wide ? 2 : 1);
            const auto pattern = read_string(filter);
            if (i == index) request.filter = pattern;
            filter += (string_units(pattern, wide) + 1) * (wide ? 2 : 1);
        }
        if (vfs) {
            auto dir = vfs->resolve_host_path(request.initial_directory.empty()
                ? vfs->cwd() : request.initial_directory);
            if (dir.is_ok()) request.initial_directory = dir.value();
            else request.initial_directory.clear();
        }
        const auto separator = request.default_name.find_last_of("\\/");
        if (separator != std::string::npos) request.default_name.erase(0, separator + 1);
        std::optional<std::string> selected;
        if (presenter) selected = presenter(request);
        else {
            auto result = save
                ? bridge->show_save_file_dialog(request.initial_directory, request.default_name,
                                                request.filter, request.title)
                : bridge->show_open_file_dialog(request.initial_directory, request.filter, request.title);
            if (result.ok) selected = result.selected_path;
        }
        if (!selected) { m.set_return(0u); return; }
        auto guest = guest_dialog_path(vfs, *selected);
        if (!guest) { dialog_error = 0x3002; m.set_return(0u); return; }
        const auto units = string_units(*guest, wide);
        if (units + 1 > capacity) {
            dialog_error = 0x3003; // FNERR_BUFFERTOOSMALL
            (void)m.write_u16(file, static_cast<u16>(units + 1));
            m.set_return(0u);
            return;
        }
        auto written = wide ? m.write_wide_string(file, *guest) : m.write_ansi_string(file, *guest);
        if (written.is_err()) { dialog_error = 0xFFFF; m.set_return(0u); return; }
        const auto last_sep = guest->find_last_of("\\/");
        const auto filename = last_sep == std::string::npos ? 0 : last_sep + 1;
        const auto dot = guest->find_last_of('.');
        const auto extension = dot != std::string::npos && dot >= filename ? dot + 1 : 0;
        (void)m.write_u16(ofn + (x64 ? 100 : 56),
            static_cast<u16>(string_units(guest->substr(0, filename), wide)));
        (void)m.write_u16(ofn + (x64 ? 102 : 58),
            static_cast<u16>(string_units(guest->substr(0, extension), wide)));
        (void)m.write_u32(ofn + index_offset, index);
        // lpstrFileTitle / nMaxFileTitle: the name and extension without the
        // path, when the caller supplied a buffer that holds it.
        const GuestAddr title_buffer = pointer(36, 64);
        const u32 title_capacity = m.read_u32(ofn + (x64 ? 72 : 40)).value_or(0);
        const std::string title_text = guest->substr(filename);
        if (title_buffer != 0 && string_units(title_text, wide) + 1 <= title_capacity) {
            (void)(wide ? m.write_wide_string(title_buffer, title_text)
                        : m.write_ansi_string(title_buffer, title_text));
        }
        m.set_return(1u);
    };
    exports.entries.push_back({"GetOpenFileNameW", 0,
        [file_dialog](Marshaler& m) { file_dialog(m, false, true); }, 4});
    exports.entries.push_back({"GetOpenFileNameA", 0,
        [file_dialog](Marshaler& m) { file_dialog(m, false, false); }, 4});
    exports.entries.push_back({"GetSaveFileNameW", 0,
        [file_dialog](Marshaler& m) { file_dialog(m, true, true); }, 4});
    exports.entries.push_back({"GetSaveFileNameA", 0,
        [file_dialog](Marshaler& m) { file_dialog(m, true, false); }, 4});
    // BOOL ChooseColorA/W(LPCHOOSECOLOR)
    // Learn ChooseColor / CHOOSECOLOR: lStructSize must be sizeof(CHOOSECOLOR)
    // (36 x86 / 72 x64, from commdlg.h) or CDERR_STRUCTSIZE; CC_RGBINIT seeds
    // the panel with rgbResult (black otherwise); OK writes rgbResult and
    // returns TRUE; Cancel returns FALSE with CommDlgExtendedError 0. The
    // panel is the native NSColorPanel; lpCustColors is left as it was,
    // because the panel offers no way to edit the 16 custom slots. A hook or
    // template dialog is guest UI the native panel cannot host: a missing
    // hook is CDERR_NOHOOK, a missing template CDERR_NOTEMPLATE, and a real
    // one fails loudly (CDERR_DIALOGFAILURE) instead of being dropped.
    auto choose_color = [bridge, color_presenter](Marshaler& m, bool wide) {
        constexpr u32 CC_RGBINIT = 0x1;
        constexpr u32 CC_ENABLEHOOK = 0x10;
        constexpr u32 CC_ENABLETEMPLATE = 0x20;
        constexpr u32 CC_ENABLETEMPLATEHANDLE = 0x40;
        constexpr u32 CDERR_STRUCTSIZE = 0x0001;
        constexpr u32 CDERR_NOTEMPLATE = 0x0003;
        constexpr u32 CDERR_NOHOOK = 0x000B;
        constexpr u32 CDERR_DIALOGFAILURE = 0xFFFF;
        dialog_error = 0;
        const GuestAddr cc = m.arg_ptr(0);
        const bool x64 = m.cpu_is_64bit();
        if (cc == 0) {
            dialog_error = CDERR_STRUCTSIZE;
            m.set_return(0u);
            return;
        }
        const u32 size = m.read_u32(cc).value_or(0);
        if (size != (x64 ? 72u : 36u)) {
            dialog_error = CDERR_STRUCTSIZE;
            m.set_return(0u);
            return;
        }
        const u32 rgb_offset = x64 ? 24 : 12;
        const u32 flags = m.read_u32(cc + (x64 ? 40 : 20)).value_or(0);
        const GuestAddr instance = m.read_ptr(cc + (x64 ? 16 : 8)).value_or(0);
        const GuestAddr hook = m.read_ptr(cc + (x64 ? 56 : 28)).value_or(0);
        const GuestAddr template_name = m.read_ptr(cc + (x64 ? 64 : 32)).value_or(0);
        if ((flags & CC_ENABLEHOOK) != 0 && hook == 0) {
            dialog_error = CDERR_NOHOOK;
            m.set_return(0u);
            return;
        }
        if (((flags & CC_ENABLETEMPLATEHANDLE) != 0 && instance == 0) ||
            ((flags & CC_ENABLETEMPLATE) != 0 &&
             (flags & CC_ENABLETEMPLATEHANDLE) == 0 &&
             (instance == 0 || template_name == 0))) {
            dialog_error = CDERR_NOTEMPLATE;
            m.set_return(0u);
            return;
        }
        if ((flags & (CC_ENABLEHOOK | CC_ENABLETEMPLATE | CC_ENABLETEMPLATEHANDLE)) != 0) {
            static std::once_flag once;
            std::call_once(once, [wide] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comdlg32!ChooseColor%c: "
                                     "hook/template dialog (guest-drawn color dialog)\n",
                             wide ? 'W' : 'A');
            });
            dialog_error = CDERR_DIALOGFAILURE;
            m.set_return(0u);
            return;
        }
        const u32 initial = (flags & CC_RGBINIT) != 0
            ? (m.read_u32(cc + rgb_offset).value_or(0) & 0x00FFFFFFu)
            : 0u;
        std::optional<u32> chosen;
        if (color_presenter) {
            chosen = color_presenter(initial);
        } else if (bridge != nullptr) {
            const auto result = bridge->show_color_dialog(initial);
            if (result.ok) chosen = result.colorref;
        } else {
            dialog_error = CDERR_DIALOGFAILURE;
            m.set_return(0u);
            return;
        }
        if (!chosen) {
            m.set_return(0u);
            return;
        }
        (void)m.write_u32(cc + rgb_offset, *chosen & 0x00FFFFFFu);
        m.set_return(1u);
    };
    exports.entries.push_back({"ChooseColorA", 0,
        [choose_color](Marshaler& m) { choose_color(m, false); }, 4});
    exports.entries.push_back({"ChooseColorW", 0,
        [choose_color](Marshaler& m) { choose_color(m, true); }, 4});

    exports.entries.push_back({"CommDlgExtendedError", 0,
        [](Marshaler& m) { m.set_return(dialog_error); }, 0});

    // short GetFileTitleW(LPCWSTR lpszFile, LPWSTR Buf, WORD cchSize)
    exports.entries.push_back(ExportEntry{
        "GetFileTitleW", 0,
        [](Marshaler& m) {
            GuestAddr file_ptr = m.arg_ptr(0);
            GuestAddr buf_ptr = m.arg_ptr(1);
            u32 cch = static_cast<u32>(m.arg(2) & 0xFFFF);
            if (file_ptr == 0) {
                m.set_return(static_cast<u64>(static_cast<u32>(-1)));
                return;
            }
            auto path = m.read_wide_string(file_ptr, 4096);
            if (path.is_err()) {
                m.set_return(static_cast<u64>(static_cast<u32>(-1)));
                return;
            }
            std::string s = path.value();
            auto pos = s.find_last_of("\\/");
            std::string filename = (pos != std::string::npos) ? s.substr(pos + 1) : s;
            if (buf_ptr != 0 && cch > 0) {
                if (filename.size() + 1 > cch) {
                    m.set_return(static_cast<u64>(static_cast<u32>(filename.size() + 1)));
                    return;
                }
                (void)m.write_wide_string(buf_ptr, filename);
            }
            VITREO_TRACE("[comdlg32] GetFileTitleW('%s') -> '%s'\n", s.c_str(), filename.c_str());
            m.set_return(0);
        },
        12
    });

    // short GetFileTitleA(LPCSTR lpszFile, LPSTR Buf, WORD cchSize)
    exports.entries.push_back(ExportEntry{
        "GetFileTitleA", 0,
        [](Marshaler& m) {
            GuestAddr file_ptr = m.arg_ptr(0);
            GuestAddr buf_ptr = m.arg_ptr(1);
            u32 cch = static_cast<u32>(m.arg(2) & 0xFFFF);
            if (file_ptr == 0) {
                m.set_return(static_cast<u64>(static_cast<u32>(-1)));
                return;
            }
            auto path = m.read_ansi_string(file_ptr, 4096);
            if (path.is_err()) {
                m.set_return(static_cast<u64>(static_cast<u32>(-1)));
                return;
            }
            std::string s = path.value();
            auto pos = s.find_last_of("\\/");
            std::string filename = (pos != std::string::npos) ? s.substr(pos + 1) : s;
            if (buf_ptr != 0 && cch > 0) {
                if (filename.size() + 1 > cch) {
                    m.set_return(static_cast<u64>(static_cast<u32>(filename.size() + 1)));
                    return;
                }
                (void)m.write_ansi_string(buf_ptr, filename);
            }
            VITREO_TRACE("[comdlg32] GetFileTitleA('%s') -> '%s'\n", s.c_str(), filename.c_str());
            m.set_return(0);
        },
        12
    });

    return exports;
}

} // namespace vitreo::winapi
