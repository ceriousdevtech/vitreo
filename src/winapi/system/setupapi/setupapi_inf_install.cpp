// SPDX-License-Identifier: LGPL-2.1-only

/// @file setupapi_inf_install.cpp
/// @brief The INF half of SetupAPI: SetupOpenInfFile, SetupGetInfInformation
///        and its two query helpers, SetupInstallFromInfSection (file and
///        registry directives), the default queue callback and
///        SetupIterateCabinet.
///
/// Contract: the Learn pages for each function and the INF directive pages
/// (CopyFiles, DelFiles, RenFiles, AddReg, DelReg, DestinationDirs,
/// SourceDisksNames/Files), SetupAPI.h (SDK 10.0.26100) for numeric values
/// and struct layouts, and tests/winapi/setupapi/oracle/setupapi_inf_probe.c
/// for everything those leave open. The probe fixed: the notification
/// sequence and the parameters of each notification; one NEEDMEDIA per
/// source medium; ENDCOPY after a skipped STARTCOPY; ENDQUEUE(FALSE) with no
/// ENDSUBQUEUE after an aborted NEEDMEDIA; the default callback's no-UI
/// answers; a missing section installing nothing successfully; the
/// SP_INF_INFORMATION byte layout; and SetupIterateCabinet's notification
/// fields and error values.

#include "winapi/system/setupapi/setupapi.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/compression/cabinet.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/system/setupapi/inf_file.hpp"
#include "winapi/system/setupapi/setupapi_internal.hpp"
#include "winapi/system/version/version.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <string>

namespace vitreo::winapi {

using namespace setupapi_internal;
using setupapi_inf::InfFile;
using setupapi_inf::InfLine;
using setupapi_inf::iequals;

namespace setupapi_internal {

/// An open HINF.
struct OpenInf {
    InfFile inf;
    std::string path;        ///< full path as opened
    u64 last_write = 0;      ///< FILETIME
};

}  // namespace setupapi_internal

namespace {

// ---- SetupAPI.h ----------------------------------------------------------------
constexpr u32 INFINFO_INF_SPEC_IS_HINF = 1;
constexpr u32 INFINFO_INF_NAME_IS_ABSOLUTE = 2;
constexpr u32 INFINFO_DEFAULT_SEARCH = 3;
constexpr u32 INFINFO_REVERSE_DEFAULT_SEARCH = 4;
constexpr u32 INFINFO_INF_PATH_LIST_SEARCH = 5;

constexpr u32 SPINST_LOGCONFIG = 0x001;
constexpr u32 SPINST_INIFILES = 0x002;
constexpr u32 SPINST_REGISTRY = 0x004;
constexpr u32 SPINST_INI2REG = 0x008;
constexpr u32 SPINST_FILES = 0x010;
constexpr u32 SPINST_BITREG = 0x020;
constexpr u32 SPINST_REGSVR = 0x040;
constexpr u32 SPINST_UNREGSVR = 0x080;
constexpr u32 SPINST_PROFILEITEMS = 0x100;
constexpr u32 SPINST_COPYINF = 0x200;
constexpr u32 SPINST_PROPERTIES = 0x400;

constexpr u32 SPFILENOTIFY_STARTQUEUE = 0x01;
constexpr u32 SPFILENOTIFY_ENDQUEUE = 0x02;
constexpr u32 SPFILENOTIFY_STARTSUBQUEUE = 0x03;
constexpr u32 SPFILENOTIFY_ENDSUBQUEUE = 0x04;
constexpr u32 SPFILENOTIFY_STARTDELETE = 0x05;
constexpr u32 SPFILENOTIFY_ENDDELETE = 0x06;
constexpr u32 SPFILENOTIFY_DELETEERROR = 0x07;
constexpr u32 SPFILENOTIFY_STARTRENAME = 0x08;
constexpr u32 SPFILENOTIFY_ENDRENAME = 0x09;
constexpr u32 SPFILENOTIFY_RENAMEERROR = 0x0a;
constexpr u32 SPFILENOTIFY_STARTCOPY = 0x0b;
constexpr u32 SPFILENOTIFY_ENDCOPY = 0x0c;
constexpr u32 SPFILENOTIFY_COPYERROR = 0x0d;
constexpr u32 SPFILENOTIFY_NEEDMEDIA = 0x0e;
constexpr u32 SPFILENOTIFY_CABINETINFO = 0x10;
constexpr u32 SPFILENOTIFY_FILEINCABINET = 0x11;
constexpr u32 SPFILENOTIFY_FILEEXTRACTED = 0x13;
constexpr u32 SPFILENOTIFY_FILEOPDELAYED = 0x14;
constexpr u32 SPFILENOTIFY_LANGMISMATCH = 0x00010000;
constexpr u32 SPFILENOTIFY_TARGETEXISTS = 0x00020000;
constexpr u32 SPFILENOTIFY_TARGETNEWER = 0x00040000;

constexpr u32 FILEOP_COPY = 0;
constexpr u32 FILEOP_RENAME = 1;
constexpr u32 FILEOP_DELETE = 2;
constexpr u32 FILEOP_ABORT = 0;
constexpr u32 FILEOP_DOIT = 1;
constexpr u32 FILEOP_SKIP = 2;
constexpr u32 FILEOP_RETRY = FILEOP_DOIT;
constexpr u32 FILEOP_NEWPATH = 4;

constexpr u32 SP_COPY_REPLACEONLY = 0x0000002;
constexpr u32 SP_COPY_NEWER = 0x0000004;
constexpr u32 SP_COPY_NOOVERWRITE = 0x0000008;
constexpr u32 SP_COPY_SOURCE_ABSOLUTE = 0x0000040;
constexpr u32 SP_COPY_IN_USE_NEEDS_REBOOT = 0x0000100;
constexpr u32 SP_COPY_FORCE_NEWER = 0x0002000;
constexpr u32 SP_COPY_NEWER_ONLY = 0x0010000;
constexpr u32 SP_COPY_NOSKIP = 0x0000400;
constexpr u32 SP_COPY_FORCE_NOOVERWRITE = 0x0001000;
constexpr u32 SP_COPY_WARNIFSKIP = 0x0004000;
constexpr u32 SP_COPY_NOBROWSE = 0x0008000;
constexpr u32 SP_COPY_SOURCEPATH_ABSOLUTE = 0x0000080;
/// The CopyFlags bits this engine honours: the UI hints the callback sees in
/// FILEPATHS.Flags / SOURCE_MEDIA.Flags, the existence and version rules,
/// and two the probe showed change nothing on this path
/// (SP_COPY_SOURCE_ABSOLUTE for a section install; SP_COPY_IN_USE_NEEDS_REBOOT,
/// which only governs a target that is in use, and no target is). A caller
/// asking for any other SP_COPY_* (language checks, forced in-use handling,
/// delete-source) fails loudly instead of having it ignored.
constexpr u32 kSupportedCopyStyle =
    SP_COPY_NOSKIP | SP_COPY_WARNIFSKIP | SP_COPY_NOBROWSE | SP_COPY_FORCE_NOOVERWRITE |
    SP_COPY_REPLACEONLY | SP_COPY_SOURCEPATH_ABSOLUTE | SP_COPY_NEWER | SP_COPY_NOOVERWRITE |
    SP_COPY_SOURCE_ABSOLUTE | SP_COPY_IN_USE_NEEDS_REBOOT | SP_COPY_FORCE_NEWER | SP_COPY_NEWER_ONLY;
/// SOURCE_MEDIA.Flags is "a subset of SP_COPY_xxx" (SetupAPI.h): the probe
/// gave 0 for a copy styled 0x300, i.e. only these pass through.
constexpr u32 kMediaFlagMask = SP_COPY_NOSKIP | SP_COPY_WARNIFSKIP | SP_COPY_NOBROWSE;

constexpr u32 ERROR_WRONG_INF_STYLE = 0xE0000100u;
constexpr u32 ERROR_SECTION_NOT_FOUND = 0xE0000101u;
constexpr u32 ERROR_LINE_NOT_FOUND = 0xE0000102u;

constexpr u32 kMaxPath = 260;
constexpr u32 kScratchSize = 0x10000;

// ---- registry roots (winreg.h) ------------------------------------------------
constexpr u64 HKEY_CLASSES_ROOT_VALUE = 0x80000000u;
constexpr u64 HKEY_CURRENT_USER_VALUE = 0x80000001u;
constexpr u64 HKEY_USERS_VALUE = 0x80000003u;

constexpr u32 REG_NONE = 0, REG_SZ = 1, REG_EXPAND_SZ = 2, REG_BINARY = 3, REG_DWORD = 4,
              REG_MULTI_SZ = 7, REG_QWORD = 11;

constexpr u32 FLG_ADDREG_BINVALUETYPE = 0x00000001;
constexpr u32 FLG_ADDREG_NOCLOBBER = 0x00000002;
constexpr u32 FLG_ADDREG_DELVAL = 0x00000004;
constexpr u32 FLG_ADDREG_APPEND = 0x00000008;
constexpr u32 FLG_ADDREG_KEYONLY = 0x00000010;
constexpr u32 FLG_ADDREG_OVERWRITEONLY = 0x00000020;
constexpr u32 FLG_ADDREG_64BITKEY = 0x00001000;
constexpr u32 FLG_ADDREG_KEYONLY_COMMON = 0x00002000;
constexpr u32 FLG_ADDREG_32BITKEY = 0x00004000;
constexpr u32 FLG_ADDREG_TYPE_MULTI_SZ = 0x00010000;
constexpr u32 FLG_ADDREG_TYPE_EXPAND_SZ = 0x00020000;
constexpr u32 FLG_ADDREG_TYPE_DWORD = 0x00010001;
constexpr u32 FLG_ADDREG_TYPE_NONE = 0x00020001;
constexpr u32 FLG_ADDREG_TYPE_QWORD = 0x000B0001;
constexpr u32 FLG_DELREG_KEYONLY_COMMON = FLG_ADDREG_KEYONLY_COMMON;
constexpr u32 FLG_DELREG_32BITKEY = FLG_ADDREG_32BITKEY;
constexpr u32 FLG_DELREG_64BITKEY = FLG_ADDREG_64BITKEY;
constexpr u32 FLG_DELREG_MULTI_SZ_DELSTRING = 0x00018002;

constexpr const char* kWindows = "C:\\Windows";

// ---- strings ------------------------------------------------------------------

[[nodiscard]] std::string acp_to_utf8(std::string_view text) {
    if (text.empty()) return {};
    const int n = StringConverter::multi_byte_to_wide_char(
        0, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return std::string(text);
    std::vector<u16> wide(static_cast<usize>(n));
    (void)StringConverter::multi_byte_to_wide_char(0, 0, text.data(), static_cast<int>(text.size()),
                                                   wide.data(), n);
    return utf16_to_utf8(wide.data(), n);
}

[[nodiscard]] std::optional<std::string> read_text(Marshaler& m, GuestAddr ptr, bool wide) {
    if (ptr == 0) return std::nullopt;
    auto value = wide ? m.read_wide_string(ptr) : m.read_ansi_string(ptr);
    if (value.is_err()) return std::nullopt;
    return wide ? value.value() : acp_to_utf8(value.value());
}

/// Characters, NUL included, @p text takes in the A or W form.
[[nodiscard]] u32 text_units(std::string_view text, bool wide) {
    return static_cast<u32>((wide ? utf8_to_utf16(text).size() : utf8_to_acp(text).size()) + 1);
}

[[nodiscard]] bool write_text(Marshaler& m, GuestAddr at, std::string_view text, bool wide) {
    return (wide ? m.write_wide_string(at, text) : m.write_ansi_string(at, utf8_to_acp(text))).is_ok();
}

[[nodiscard]] std::string parent_dir(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    return pos == std::string::npos ? std::string{} : path.substr(0, pos);
}

/// A number as INF fields write them: 0x-prefixed hex, else decimal (a
/// leading '-' for dirid -1). nullopt for anything else.
[[nodiscard]] std::optional<u64> parse_wide_number(std::string_view text, int radix = 0) {
    text = text.substr(0, text.find_first_of(" \t"));
    if (text.empty()) return std::nullopt;
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const bool negative = copy.front() == '-';
    const unsigned long long v = negative ? static_cast<unsigned long long>(std::strtoll(copy.c_str(), &end, radix))
                                          : std::strtoull(copy.c_str(), &end, radix);
    if (errno != 0 || end != copy.c_str() + copy.size()) return std::nullopt;
    return static_cast<u64>(v);
}

[[nodiscard]] std::optional<u32> parse_number(std::string_view text) {
    const auto v = parse_wide_number(text);
    if (!v) return std::nullopt;
    return static_cast<u32>(*v);
}

// ---- guest scratch ring and callbacks -----------------------------------------

class Scratch {
public:
    Scratch(Marshaler& m, const StatePtr& state) : m_(m), state_(state) {}

    [[nodiscard]] GuestAddr alloc(u32 bytes) {
        std::lock_guard lock(state_->mutex);
        if (state_->scratch_base == 0) {
            if (state_->services == nullptr || !state_->services->reserve) return 0;
            state_->scratch_base = state_->services->reserve(kScratchSize);
            if (state_->scratch_base == 0) return 0;
        }
        const u32 aligned = (bytes + 15u) & ~15u;
        // Records live for one notification, so the ring may wrap.
        if (state_->scratch_cursor + aligned > kScratchSize) state_->scratch_cursor = 0;
        const GuestAddr out = state_->scratch_base + state_->scratch_cursor;
        state_->scratch_cursor += aligned;
        return out;
    }

    /// A NUL-terminated A or W copy of @p text; 0 for a null pointer.
    [[nodiscard]] GuestAddr text(const std::optional<std::string>& text, bool wide) {
        if (!text) return 0;
        const GuestAddr at = alloc(text_units(*text, wide) * (wide ? 2u : 1u));
        if (at == 0 || !write_text(m_, at, *text, wide)) return 0;
        return at;
    }

    /// A zeroed MAX_PATH character buffer (NEEDMEDIA / COPYERROR Param2).
    [[nodiscard]] GuestAddr path_buffer(bool wide) {
        const u32 bytes = kMaxPath * (wide ? 2u : 1u);
        const GuestAddr at = alloc(bytes);
        for (u32 i = 0; at != 0 && i < bytes; i += 4) (void)m_.write_u32(at + i, 0);
        return at;
    }

private:
    Marshaler& m_;
    const StatePtr& state_;
};

[[nodiscard]] bool write_ptr_field(Marshaler& m, GuestAddr at, u64 value, bool is64) {
    return is64 ? m.write_u64(at, value).is_ok() : m.write_u32(at, static_cast<u32>(value)).is_ok();
}

[[nodiscard]] u64 read_ptr_field(Marshaler& m, GuestAddr at, bool is64) {
    if (is64) {
        const auto v = m.read_u64(at);
        return v.is_ok() ? v.value() : 0;
    }
    const auto v = m.read_u32(at);
    return v.is_ok() ? v.value() : 0;
}

/// FILEPATHS_A/W: Target, Source, Win32Error, Flags.
[[nodiscard]] GuestAddr make_filepaths(Marshaler& m, Scratch& scratch, bool wide,
                                       const std::string& target,
                                       const std::optional<std::string>& source, u32 error,
                                       u32 flags) {
    const bool is64 = m.cpu_is_64bit();
    const u32 p = is64 ? 8 : 4;
    const GuestAddr record = scratch.alloc(2 * p + 8);
    if (record == 0) return 0;
    (void)write_ptr_field(m, record, scratch.text(target, wide), is64);
    (void)write_ptr_field(m, record + p, scratch.text(source, wide), is64);
    (void)m.write_u32(record + 2 * p, error);
    (void)m.write_u32(record + 2 * p + 4, flags);
    return record;
}

class Notifier {
public:
    Notifier(Marshaler& m, const StatePtr& state, GuestAddr handler, GuestAddr context, bool wide)
        : m_(m), state_(state), handler_(handler), context_(context), wide_(wide) {}

    [[nodiscard]] bool usable() const {
        return handler_ != 0 && state_->services != nullptr && state_->services->call;
    }

    u32 operator()(u32 note, u64 p1, u64 p2) {
        const u64 args[4] = {context_, note, p1, p2};
        return static_cast<u32>(state_->services->call(handler_, args, 4, m_.cpu_is_64bit()));
    }

    [[nodiscard]] bool wide() const { return wide_; }

private:
    Marshaler& m_;
    const StatePtr& state_;
    GuestAddr handler_;
    GuestAddr context_;
    bool wide_;
};

// ---- dirids ------------------------------------------------------------------

/// The directory a dirid names (Learn, Using Dirids), spelled as the probe
/// recorded %dirid% expansions: under the Windows directory "system32",
/// "system32\DRIVERS", "INF"; 16422 is the caller's Program Files (the x86
/// one in a 32-bit process); -1 is the empty string. 01 is @p source_root.
[[nodiscard]] std::optional<std::string> dirid_path(i64 dirid, const std::string& source_root, bool is64) {
    const std::string win = kWindows;
    switch (dirid) {
        case -1: case 65535: return std::string{};
        case 1: return source_root;
        case 10: return win;
        case 11: return win + "\\system32";
        case 12: return win + "\\system32\\DRIVERS";
        case 17: return win + "\\INF";
        case 18: return win + "\\Help";
        case 20: return win + "\\Fonts";
        case 24: return std::string("C:\\");
        case 50: return win + "\\system";
        case 16422: return std::string(is64 ? "C:\\Program Files" : "C:\\Program Files (x86)");
        case 16425: return win + "\\SysWOW64";
        case 16426: return std::string("C:\\Program Files (x86)");
        case 16427: return std::string(is64 ? "C:\\Program Files\\Common Files"
                                            : "C:\\Program Files (x86)\\Common Files");
        case 16428: return std::string("C:\\Program Files (x86)\\Common Files");
        default: return std::nullopt;
    }
}

// ---- the queue ------------------------------------------------------------------

struct SourceMedium {
    std::string description;
    std::string tagfile;  ///< tag-or-cab-file
    std::string path;     ///< directory the files and tag/cab are in
    /// SourceDisksNames flags with 0x10 (the DirectX INFs write -1): the
    /// tag-or-cab-file is a cabinet the files come from (Learn, format 2).
    bool cab_format = false;
};

struct QueuedOp {
    u32 op = FILEOP_COPY;
    std::string target;          ///< full target path
    std::string old_target;      ///< RenFiles: the file being renamed
    std::string source_dir;
    std::string source_name;
    std::optional<usize> medium; ///< index into Queue::media
    u32 style = 0;
};

struct Queue {
    std::vector<SourceMedium> media;
    std::vector<QueuedOp> deletes, renames, copies;
};

struct BuildError {
    u32 error;
};

class InfInstaller {
public:
    InfInstaller(const OpenInf& open, std::string source_root, u32 copy_style, bool is64)
        : inf_(open.inf), root_(std::move(source_root)),
          copy_style_(copy_style), is64_(is64) {}

    /// Queue every CopyFiles/DelFiles/RenFiles of @p section.
    [[nodiscard]] std::optional<BuildError> queue_files(const setupapi_inf::InfSection& section,
                                                        Queue& q) {
        for (const auto& line : section.lines) {
            const std::string directive = inf_.key(line);
            const bool copy = iequals(directive, "CopyFiles");
            const bool del = iequals(directive, "DelFiles");
            const bool ren = iequals(directive, "RenFiles");
            if (!copy && !del && !ren) continue;
            for (usize f = 1; f <= inf_.field_count(line); ++f) {
                const std::string list = *inf_.field(line, f);
                if (list.empty()) continue;
                if (copy && list.front() == '@') {
                    auto dir = destination(std::nullopt);
                    if (!dir) return BuildError{ERROR_LINE_NOT_FOUND};
                    if (auto e = queue_copy(q, *dir, list.substr(1), list.substr(1), 0)) return e;
                    continue;
                }
                const auto* files = inf_.section(list);
                if (files == nullptr) return BuildError{ERROR_SECTION_NOT_FOUND};
                auto dir = destination(list);
                if (!dir) return BuildError{ERROR_LINE_NOT_FOUND};
                for (const auto& entry : files->lines) {
                    const std::string first =
                        entry.has_key ? inf_.key(entry) : inf_.field(entry, 1).value_or("");
                    if (first.empty()) continue;
                    if (copy) {
                        // destination-file-name[,[source-file-name][,[unused][,flag]]]
                        std::string source = entry.has_key ? std::string{} : inf_.field(entry, 2).value_or("");
                        if (source.empty()) source = first;
                        const std::string flag_text = inf_.field(entry, 4).value_or("");
                        const u32 flags = flag_text.empty() ? 0 : parse_number(flag_text).value_or(0xFFFFFFFFu);
                        if (auto e = queue_copy(q, *dir, first, source, flags)) return e;
                    } else if (del) {
                        const std::string flag_text = inf_.field(entry, 4).value_or("");
                        if (!flag_text.empty() && parse_number(flag_text).value_or(1) != 0) {
                            log_unimplemented_once("setupapi!SetupInstallFromInfSection",
                                                   "DelFiles DELFLG_IN_USE (delete at reboot)");
                            return BuildError{error_codes::ERROR_CALL_NOT_IMPLEMENTED};
                        }
                        q.deletes.push_back(QueuedOp{FILEOP_DELETE, join_win_path(*dir, first), {}, {}, {}, {}, 0});
                    } else {
                        // new-dest-file-name,old-source-file-name
                        const std::string old_name = inf_.field(entry, 2).value_or("");
                        if (old_name.empty()) return BuildError{error_codes::ERROR_INVALID_DATA};
                        q.renames.push_back(QueuedOp{FILEOP_RENAME, join_win_path(*dir, first),
                                                     join_win_path(*dir, old_name), {}, {}, {}, 0});
                    }
                }
            }
        }
        return std::nullopt;
    }

private:
    /// [DestinationDirs] entry for @p list, else DefaultDestDir, else
    /// DIRID_DEFAULT (= DIRID_SYSTEM, SetupAPI.h). nullopt for a dirid this
    /// engine cannot place (logged).
    [[nodiscard]] std::optional<std::string> destination(const std::optional<std::string>& list) {
        const auto* dirs = inf_.section("DestinationDirs");
        const InfLine* hit = nullptr;
        if (dirs != nullptr) {
            for (const auto& line : dirs->lines) {
                const std::string key = inf_.key(line);
                if (list && iequals(key, *list)) { hit = &line; break; }
            }
            if (hit == nullptr) {
                for (const auto& line : dirs->lines) {
                    if (iequals(inf_.key(line), "DefaultDestDir")) { hit = &line; break; }
                }
            }
        }
        if (hit == nullptr) return dirid_path(11, root_, is64_);
        const auto id = parse_number(inf_.field(*hit, 1).value_or(""));
        if (!id) return std::nullopt;
        const i64 dirid = static_cast<i32>(*id);
        const std::string subdir = inf_.field(*hit, 2).value_or("");
        if (dirid == -1 || dirid == 65535) return subdir;
        auto base = dirid_path(dirid, root_, is64_);
        if (!base) {
            char what[64];
            std::snprintf(what, sizeof what, "DestinationDirs dirid %lld", static_cast<long long>(dirid));
            log_unimplemented_once("setupapi!SetupInstallFromInfSection", what);
            return std::nullopt;
        }
        return subdir.empty() ? *base : join_win_path(*base, subdir);
    }

    /// The platform-decorated section first (.x86 / .amd64), then the plain one.
    [[nodiscard]] const InfLine* find_keyed(const char* section, std::string_view key) const {
        const std::string decorated = std::string(section) + (is64_ ? ".amd64" : ".x86");
        for (const char* name : {decorated.c_str(), section}) {
            if (const auto* s = inf_.section(name)) {
                for (const auto& line : s->lines) {
                    if (iequals(inf_.key(line), key)) return &line;
                }
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<BuildError> queue_copy(Queue& q, const std::string& dir,
                                                       const std::string& target,
                                                       const std::string& source, u32 flags) {
        if (flags != 0) {
            // COPYFLG_* (Learn, CopyFiles): each changes overwrite, version
            // or in-use handling this engine does not implement.
            char what[64];
            std::snprintf(what, sizeof what, "CopyFiles flag 0x%x", flags);
            log_unimplemented_once("setupapi!SetupInstallFromInfSection", what);
            return BuildError{error_codes::ERROR_CALL_NOT_IMPLEMENTED};
        }
        QueuedOp op;
        op.op = FILEOP_COPY;
        op.target = join_win_path(dir, target);
        op.source_name = source;
        op.style = copy_style_;
        std::string medium_path = root_;
        std::string subdir;
        if (const InfLine* file = find_keyed("SourceDisksFiles", source)) {
            // filename=diskid[,[subdir][,size]]
            const std::string disk = inf_.field(*file, 1).value_or("");
            subdir = inf_.field(*file, 2).value_or("");
            if (const InfLine* name = find_keyed("SourceDisksNames", disk)) {
                // diskid = disk-description[,[tag-or-cab-file],[unused],[path],[flags][,tag-file]]
                SourceMedium medium;
                medium.description = inf_.field(*name, 1).value_or("");
                medium.tagfile = inf_.field(*name, 2).value_or("");
                std::string path = inf_.field(*name, 4).value_or("");
                while (!path.empty() && (path.front() == '\\' || path.front() == '/')) path.erase(0, 1);
                if (!path.empty() && (copy_style_ & SP_COPY_SOURCEPATH_ABSOLUTE) == 0) {
                    medium_path = join_win_path(root_, path);
                }
                const std::string media_flags = inf_.field(*name, 5).value_or("");
                const u32 media_bits = media_flags.empty() ? 0 : parse_number(media_flags).value_or(1);
                medium.cab_format = (media_bits & 0x10u) != 0;
                if (media_bits != 0 && !medium.cab_format) {
                    // Learn: values other than 0x10 are "internal use only".
                    log_unimplemented_once("setupapi!SetupInstallFromInfSection",
                                           "SourceDisksNames flags other than 0x10");
                    return BuildError{error_codes::ERROR_CALL_NOT_IMPLEMENTED};
                }
                if (medium.cab_format && !inf_.field(*name, 6).value_or("").empty()) {
                    log_unimplemented_once("setupapi!SetupInstallFromInfSection",
                                           "SourceDisksNames separate tag-file");
                    return BuildError{error_codes::ERROR_CALL_NOT_IMPLEMENTED};
                }
                medium.path = medium_path;
                auto same = std::find_if(q.media.begin(), q.media.end(), [&](const SourceMedium& s) {
                    return s.path == medium.path && iequals(s.tagfile, medium.tagfile) &&
                           s.description == medium.description;
                });
                if (same == q.media.end()) {
                    q.media.push_back(medium);
                    op.medium = q.media.size() - 1;
                } else {
                    op.medium = static_cast<usize>(same - q.media.begin());
                }
            }
        }
        op.source_dir = subdir.empty() ? medium_path : join_win_path(medium_path, subdir);
        q.copies.push_back(std::move(op));
        return std::nullopt;
    }

    const InfFile& inf_;
    std::string root_;
    u32 copy_style_;
    bool is64_;
};

/// Bytes of one file out of a cabinet, name matched without case (CFFILE
/// names are ASCII in every cabinet SetupAPI is given in practice).
[[nodiscard]] std::optional<std::vector<u8>> extract_from_cab(capsule::Vfs& vfs, const std::string& cab_path,
                                                              const std::string& name) {
    std::vector<u8> image;
    if (!vfs_read_all(vfs, cab_path, image)) return std::nullopt;
    auto cab = compression::cabinet::Cabinet::parse(image);
    if (cab.is_err()) return std::nullopt;
    for (const auto& file : cab.value().files()) {
        if (!iequals(file.name, name)) continue;
        auto bytes = cab.value().extract_file(file);
        if (bytes.is_err()) return std::nullopt;
        return std::move(bytes).value();
    }
    return std::nullopt;
}

[[nodiscard]] bool is_cab_name(const std::string& name) {
    return name.size() > 4 && iequals(std::string_view(name).substr(name.size() - 4), ".cab");
}

void make_dirs(capsule::Vfs& vfs, const std::string& dir) {
    if (dir.size() <= 3) return;
    if (vfs.stat(dir).is_ok()) return;
    make_dirs(vfs, parent_dir(dir));
    (void)vfs.create_dir(dir);
}

enum class SourceKind { Missing, Loose, Cabinet };

/// Where a queued file comes from now. A cab-format medium whose cabinet the
/// first NEEDMEDIA found (@p cab_found) serves every file; otherwise the loose
/// file, and for a plain medium a tag-or-cab-file named *.cab after it
/// (Learn, SourceDisksNames format 1). @p source is the path reported as
/// FILEPATHS.Source.
[[nodiscard]] SourceKind locate_source(capsule::Vfs& vfs, const QueuedOp& op, const SourceMedium* medium,
                                       bool cab_found, std::string& source) {
    const std::string loose = join_win_path(op.source_dir, op.source_name);
    source = loose;
    if (medium != nullptr && medium->cab_format && cab_found) {
        source = join_win_path(medium->path, medium->tagfile);
        return SourceKind::Cabinet;
    }
    if (vfs_is_file(vfs, loose)) return SourceKind::Loose;
    if (medium != nullptr && !medium->cab_format && is_cab_name(medium->tagfile)) {
        const std::string cab = join_win_path(medium->path, medium->tagfile);
        if (vfs_is_file(vfs, cab)) {
            source = cab;
            return SourceKind::Cabinet;
        }
    }
    return SourceKind::Missing;
}

[[nodiscard]] u32 load_source(capsule::Vfs& vfs, const QueuedOp& op, SourceKind kind,
                              const std::string& source, std::vector<u8>& bytes) {
    if (kind == SourceKind::Loose) {
        return vfs_read_all(vfs, source, bytes) ? error_codes::ERROR_SUCCESS
                                                : error_codes::ERROR_ACCESS_DENIED;
    }
    if (kind == SourceKind::Cabinet) {
        auto extracted = extract_from_cab(vfs, source, op.source_name);
        if (!extracted) return error_codes::ERROR_FILE_NOT_FOUND;
        bytes = std::move(*extracted);
        return error_codes::ERROR_SUCCESS;
    }
    return error_codes::ERROR_FILE_NOT_FOUND;
}

/// The SPFILENOTIFY_TARGET* bits a copy raises before it overwrites
/// (Learn, SetupInstallFromInfSection CopyFlags; probe "-- style" cases):
/// TARGETEXISTS for SP_COPY_NOOVERWRITE; TARGETNEWER for SP_COPY_NEWER when
/// the target's file version is higher, for SP_COPY_NEWER_ONLY when it is
/// not lower. A file without a version resource counts as older. The
/// FORCE_ forms skip without asking; @p skip is set for those.
[[nodiscard]] u32 overwrite_question(capsule::Vfs& vfs, const QueuedOp& op,
                                     const std::vector<u8>& source, bool& skip) {
    skip = false;
    const bool exists = vfs_is_file(vfs, op.target);
    if ((op.style & SP_COPY_REPLACEONLY) != 0 && !exists) {
        skip = true;
        return 0;
    }
    if (!exists) return 0;
    if ((op.style & SP_COPY_FORCE_NOOVERWRITE) != 0) {
        skip = true;
        return 0;
    }
    u32 note = 0;
    if ((op.style & SP_COPY_NOOVERWRITE) != 0) note |= SPFILENOTIFY_TARGETEXISTS;
    if ((op.style & (SP_COPY_NEWER | SP_COPY_NEWER_ONLY | SP_COPY_FORCE_NEWER)) != 0) {
        std::vector<u8> target;
        const auto target_version = vfs_read_all(vfs, op.target, target) ? fixed_file_version(target)
                                                                         : std::nullopt;
        const auto source_version = fixed_file_version(source);
        if (target_version && source_version) {
            const bool target_newer = *target_version > *source_version;
            const bool target_same = *target_version == *source_version;
            if ((op.style & SP_COPY_FORCE_NEWER) != 0 && target_newer) {
                skip = true;
                return 0;
            }
            if (((op.style & SP_COPY_NEWER) != 0 && target_newer) ||
                ((op.style & SP_COPY_NEWER_ONLY) != 0 && (target_newer || target_same))) {
                note |= SPFILENOTIFY_TARGETNEWER;
            }
        }
    }
    return note;
}

[[nodiscard]] u32 write_target(capsule::Vfs& vfs, const QueuedOp& op, const std::vector<u8>& bytes) {
    make_dirs(vfs, parent_dir(op.target));
    return vfs_write_all(vfs, op.target, bytes) ? error_codes::ERROR_SUCCESS
                                                : error_codes::ERROR_ACCESS_DENIED;
}

/// Commit @p q through @p notify: STARTQUEUE, then the delete, rename and
/// copy sub-queues (Learn: that order; RenFiles run before copies), then
/// ENDQUEUE. Returns TRUE/FALSE with the last error set as Windows leaves it.
[[nodiscard]] bool commit_queue(Marshaler& m, const StatePtr& state, Notifier& notify, u64 owner,
                                const Queue& q) {
    Scratch scratch(m, state);
    const bool wide = notify.wide();
    const bool is64 = m.cpu_is_64bit();
    capsule::Vfs& vfs = *state->vfs;
    if (notify(SPFILENOTIFY_STARTQUEUE, owner, 0) == 0) return false;
    const auto end_queue = [&](bool ok) {
        (void)notify(SPFILENOTIFY_ENDQUEUE, ok ? 1 : 0, 0);
        return ok;
    };

    // Deletes and renames.
    for (const auto& [list, op_code] : {std::pair{&q.deletes, FILEOP_DELETE}, std::pair{&q.renames, FILEOP_RENAME}}) {
        if (list->empty()) continue;
        if (notify(SPFILENOTIFY_STARTSUBQUEUE, op_code, list->size()) == 0) return end_queue(false);
        const bool del = op_code == FILEOP_DELETE;
        for (const auto& op : *list) {
            const std::optional<std::string> source =
                del ? std::nullopt : std::optional<std::string>(op.old_target);
            GuestAddr fp = make_filepaths(m, scratch, wide, op.target, source, 0, 0);
            const u32 start = notify(del ? SPFILENOTIFY_STARTDELETE : SPFILENOTIFY_STARTRENAME, fp, op_code);
            if (start == FILEOP_ABORT) return end_queue(false);
            u32 error = error_codes::ERROR_SUCCESS;
            if (start == FILEOP_DOIT) {
                for (;;) {
                    if (del) {
                        error = !vfs_is_file(vfs, op.target) ? error_codes::ERROR_FILE_NOT_FOUND
                                : vfs.remove(op.target).is_ok() ? error_codes::ERROR_SUCCESS
                                                                : error_codes::ERROR_ACCESS_DENIED;
                    } else {
                        error = !vfs_is_file(vfs, op.old_target) ? error_codes::ERROR_FILE_NOT_FOUND
                                : vfs.rename(op.old_target, op.target).is_ok() ? error_codes::ERROR_SUCCESS
                                                                               : error_codes::ERROR_ACCESS_DENIED;
                    }
                    if (error == error_codes::ERROR_SUCCESS) break;
                    fp = make_filepaths(m, scratch, wide, op.target, source, error, 0);
                    const u32 answer = notify(del ? SPFILENOTIFY_DELETEERROR : SPFILENOTIFY_RENAMEERROR, fp, 0);
                    if (answer == FILEOP_ABORT) {
                        set_last_error(error);
                        return end_queue(false);
                    }
                    if (answer != FILEOP_RETRY) break;
                }
            }
            fp = make_filepaths(m, scratch, wide, op.target, source, error, 0);
            (void)notify(del ? SPFILENOTIFY_ENDDELETE : SPFILENOTIFY_ENDRENAME, fp, 0);
        }
        (void)notify(SPFILENOTIFY_ENDSUBQUEUE, op_code, 0);
    }

    // Copies. A medium gets one NEEDMEDIA before its first file (for a
    // cab-format medium it names the cabinet, with no tag file). A file still
    // missing after that is asked for again by name until the callback
    // supplies it, skips or aborts; the probe saw no COPYERROR for a missing
    // source, only repeated NEEDMEDIA.
    if (!q.copies.empty()) {
        if (notify(SPFILENOTIFY_STARTSUBQUEUE, FILEOP_COPY, q.copies.size()) == 0) return end_queue(false);
        const u32 p = is64 ? 8 : 4;
        // NEEDMEDIA with SOURCE_MEDIA { Reserved, Tagfile, Description,
        // SourcePath, SourceFile, Flags } and a MAX_PATH Param2 buffer.
        const auto need_media = [&](const std::optional<std::string>& tag, const std::string& description,
                                    const std::string& path, const std::string& file, u32 style,
                                    std::optional<std::string>& new_path) -> std::optional<u32> {
            const GuestAddr sm = scratch.alloc(5 * p + 4);
            const GuestAddr buffer = scratch.path_buffer(wide);
            if (sm == 0 || buffer == 0) return std::nullopt;
            (void)write_ptr_field(m, sm, 0, is64);
            (void)write_ptr_field(m, sm + p, scratch.text(tag, wide), is64);
            (void)write_ptr_field(m, sm + 2 * p, scratch.text(description, wide), is64);
            (void)write_ptr_field(m, sm + 3 * p, scratch.text(path, wide), is64);
            (void)write_ptr_field(m, sm + 4 * p, scratch.text(file, wide), is64);
            (void)m.write_u32(sm + 5 * p, style & kMediaFlagMask);
            const u32 answer = notify(SPFILENOTIFY_NEEDMEDIA, sm, buffer);
            if (answer == FILEOP_NEWPATH) new_path = read_text(m, buffer, wide);
            return answer;
        };
        std::optional<usize> current_medium;
        bool first = true;
        bool medium_skipped = false;
        bool cab_found = false;
        std::optional<std::string> new_path;
        for (const auto& queued : q.copies) {
            QueuedOp op = queued;
            const SourceMedium* medium_ptr = op.medium ? &q.media[*op.medium] : nullptr;
            std::optional<SourceMedium> redirected;
            if (first || op.medium != current_medium) {
                first = false;
                current_medium = op.medium;
                medium_skipped = false;
                cab_found = false;
                new_path.reset();
                const bool cab_format = medium_ptr != nullptr && medium_ptr->cab_format;
                const std::optional<std::string> tag =
                    !cab_format && medium_ptr != nullptr && !medium_ptr->tagfile.empty()
                        ? std::optional(medium_ptr->tagfile) : std::nullopt;
                const auto answer = need_media(
                    tag, medium_ptr ? medium_ptr->description : std::string{},
                    cab_format ? medium_ptr->path : op.source_dir,
                    cab_format ? medium_ptr->tagfile : op.source_name, op.style, new_path);
                if (!answer) {
                    set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                    return end_queue(false);
                }
                if (*answer == FILEOP_ABORT) return end_queue(false);
                if (*answer == FILEOP_SKIP) medium_skipped = true;
                if (cab_format) {
                    const std::string dir = new_path && !new_path->empty() ? *new_path : medium_ptr->path;
                    cab_found = vfs_is_file(vfs, join_win_path(dir, medium_ptr->tagfile));
                }
            }
            if (medium_skipped) continue;
            if (new_path && !new_path->empty()) {
                op.source_dir = *new_path;
                if (medium_ptr != nullptr) {
                    redirected = *medium_ptr;
                    redirected->path = *new_path;
                    medium_ptr = &*redirected;
                }
            }
            std::string reported;
            SourceKind kind = locate_source(vfs, op, medium_ptr, cab_found, reported);
            bool skip_file = false;
            // A cab-format medium whose cabinet is absent asks for each file
            // by name even when the loose file is there (probe: FlagLoose
            // with NEEDMEDIA answered DOIT).
            bool ask = medium_ptr != nullptr && medium_ptr->cab_format && !cab_found;
            while (kind == SourceKind::Missing || ask) {
                ask = false;
                std::optional<std::string> path;
                const std::optional<std::string> tag =
                    medium_ptr != nullptr && !medium_ptr->cab_format && !medium_ptr->tagfile.empty()
                        ? std::optional(medium_ptr->tagfile) : std::nullopt;
                const auto answer = need_media(tag, medium_ptr ? medium_ptr->description : std::string{},
                                               op.source_dir, op.source_name, op.style, path);
                if (!answer) {
                    set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                    return end_queue(false);
                }
                if (*answer == FILEOP_ABORT) return end_queue(false);
                if (*answer == FILEOP_SKIP) {
                    skip_file = true;
                    break;
                }
                if (*answer == FILEOP_NEWPATH && path && !path->empty()) op.source_dir = *path;
                kind = locate_source(vfs, op, medium_ptr, false, reported);
            }
            if (skip_file) continue;
            GuestAddr fp = make_filepaths(m, scratch, wide, op.target, reported, 0, op.style);
            const u32 start = notify(SPFILENOTIFY_STARTCOPY, fp, FILEOP_COPY);
            if (start == FILEOP_ABORT) return end_queue(false);
            u32 error = error_codes::ERROR_SUCCESS;
            if (start == FILEOP_DOIT) {
                for (;;) {
                    std::vector<u8> bytes;
                    error = load_source(vfs, op, kind, reported, bytes);
                    if (error == error_codes::ERROR_SUCCESS) {
                        bool skip = false;
                        const u32 question = overwrite_question(vfs, op, bytes, skip);
                        if (question != 0) {
                            fp = make_filepaths(m, scratch, wide, op.target, reported, 0, op.style);
                            // TRUE overwrites, FALSE keeps the target (Learn).
                            skip = notify(question, fp, 0) == 0;
                        }
                        if (skip) break;
                        error = write_target(vfs, op, bytes);
                    }
                    if (error == error_codes::ERROR_SUCCESS) break;
                    fp = make_filepaths(m, scratch, wide, op.target, reported, error, op.style);
                    const GuestAddr buffer = scratch.path_buffer(wide);
                    const u32 answer = notify(SPFILENOTIFY_COPYERROR, fp, buffer);
                    if (answer == FILEOP_ABORT) {
                        fp = make_filepaths(m, scratch, wide, op.target, reported, error, op.style);
                        (void)notify(SPFILENOTIFY_ENDCOPY, fp, 0);
                        set_last_error(error);
                        return end_queue(false);
                    }
                    if (answer == FILEOP_NEWPATH) {
                        if (auto path = read_text(m, buffer, wide); path && !path->empty()) {
                            op.source_dir = *path;
                            kind = locate_source(vfs, op, medium_ptr, false, reported);
                        }
                        continue;
                    }
                    if (answer != FILEOP_RETRY) break;
                }
            }
            fp = make_filepaths(m, scratch, wide, op.target, reported, error, op.style);
            (void)notify(SPFILENOTIFY_ENDCOPY, fp, 0);
        }
        (void)notify(SPFILENOTIFY_ENDSUBQUEUE, FILEOP_COPY, 0);
    }
    return end_queue(true);
}

// ---- registry directives ---------------------------------------------------------

[[nodiscard]] std::optional<u64> registry_root(const std::string& name, u64 relative_root) {
    if (iequals(name, "HKCR")) return HKEY_CLASSES_ROOT_VALUE;
    if (iequals(name, "HKCU")) return HKEY_CURRENT_USER_VALUE;
    if (iequals(name, "HKLM")) return HKEY_LOCAL_MACHINE_VALUE;
    if (iequals(name, "HKU")) return HKEY_USERS_VALUE;
    if (iequals(name, "HKR") && relative_root != 0) return relative_root;
    return std::nullopt;
}

[[nodiscard]] std::vector<u8> utf16_bytes(std::string_view text, bool terminate = true) {
    std::vector<u16> wide = utf8_to_utf16(text);
    if (terminate) wide.push_back(0);
    std::vector<u8> out(wide.size() * 2);
    for (usize i = 0; i < wide.size(); ++i) {
        out[2 * i] = static_cast<u8>(wide[i]);
        out[2 * i + 1] = static_cast<u8>(wide[i] >> 8);
    }
    return out;
}

/// A 32BITKEY/64BITKEY flag naming the other bitness's view, which Vitreo's
/// single registry view cannot honour; the process's own view is the
/// native registry the flag asks for.
[[nodiscard]] bool foreign_view(u32 flags, bool is64) {
    return (is64 && (flags & FLG_ADDREG_32BITKEY) != 0) || (!is64 && (flags & FLG_ADDREG_64BITKEY) != 0);
}

[[nodiscard]] u32 apply_add_reg(RegistryThunks& reg, const InfFile& inf, const InfLine& line,
                                u64 relative_root, bool is64) {
    // reg-root,[subkey],[value-entry-name],[flags],[value][,[value]]
    // (a keyless line: SetupGetStringField fields 1..n)
    const auto root = registry_root(inf.field(line, 1).value_or(""), relative_root);
    if (!root) return error_codes::ERROR_INVALID_DATA;
    const std::string subkey = inf.field(line, 2).value_or("");
    const std::optional<std::string> name = inf.field(line, 3);
    const std::string flag_text = inf.field(line, 4).value_or("");
    const u32 flags = flag_text.empty() ? 0 : parse_number(flag_text).value_or(0xFFFFFFFFu);
    if (flags == 0xFFFFFFFFu) return error_codes::ERROR_INVALID_DATA;
    if (foreign_view(flags, is64)) {
        log_unimplemented_once("setupapi!SetupInstallFromInfSection", "AddReg on the other bitness's view");
        return error_codes::ERROR_CALL_NOT_IMPLEMENTED;
    }
    const u32 base = flags & ~(FLG_ADDREG_64BITKEY | FLG_ADDREG_32BITKEY);
    u64 key = 0;
    u32 disposition = 0;
    if ((base & FLG_ADDREG_DELVAL) != 0) {
        // Learn: delete the named value, or the subkey when no name is given;
        // an already-absent target is the requested end state.
        if (name && !name->empty()) {
            (void)reg.reg_delete_key_value_w(*root, widen(subkey), widen(*name));
        } else if (reg.reg_delete_tree(*root, widen(subkey)) == error_codes::ERROR_SUCCESS) {
            (void)reg.reg_delete_key(*root, widen(subkey));
        }
        return error_codes::ERROR_SUCCESS;
    }
    if (const u32 r = reg.reg_create_key_ex_w(*root, widen(subkey), &key, &disposition); r != 0) return r;
    struct Close {
        RegistryThunks& reg;
        u64 key;
        ~Close() { (void)reg.reg_close_key(key); }
    } close{reg, key};
    if ((base & (FLG_ADDREG_KEYONLY | FLG_ADDREG_KEYONLY_COMMON)) != 0) return error_codes::ERROR_SUCCESS;
    const std::wstring value_name = widen(name.value_or(""));
    u32 existing_type = 0;
    u32 existing_size = 0;
    const bool exists = reg.reg_query_value_ex_w(key, value_name, &existing_type, nullptr, &existing_size) == 0;
    if ((base & FLG_ADDREG_NOCLOBBER) != 0 && exists) return error_codes::ERROR_SUCCESS;
    if ((base & FLG_ADDREG_OVERWRITEONLY) != 0 && !exists) return error_codes::ERROR_SUCCESS;

    const usize first_value = 5;
    std::vector<std::string> values;
    for (usize f = first_value; f <= inf.field_count(line); ++f) values.push_back(*inf.field(line, f));
    const u32 type_bits = base & ~(FLG_ADDREG_NOCLOBBER | FLG_ADDREG_DELVAL | FLG_ADDREG_APPEND |
                                   FLG_ADDREG_OVERWRITEONLY);
    std::vector<u8> data;
    u32 type = REG_SZ;
    if (type_bits == 0 || type_bits == FLG_ADDREG_TYPE_EXPAND_SZ) {
        type = type_bits == 0 ? REG_SZ : REG_EXPAND_SZ;
        data = utf16_bytes(values.empty() ? std::string{} : values[0]);
    } else if (type_bits == FLG_ADDREG_TYPE_MULTI_SZ) {
        type = REG_MULTI_SZ;
        std::vector<std::string> strings = values;
        if ((base & FLG_ADDREG_APPEND) != 0 && exists && existing_type == REG_MULTI_SZ) {
            std::vector<u8> old(existing_size);
            u32 size = existing_size;
            (void)reg.reg_query_value_ex_w(key, value_name, &existing_type, old.data(), &size);
            std::vector<std::string> merged;
            std::string current;
            for (usize i = 0; i + 1 < size; i += 2) {
                const u16 c = static_cast<u16>(old[i] | (old[i + 1] << 8));
                if (c == 0) {
                    if (!current.empty()) merged.push_back(current);
                    current.clear();
                } else {
                    const u16 one[1] = {c};
                    current += utf16_to_utf8(one, 1);
                }
            }
            for (const auto& s : strings) {
                if (std::none_of(merged.begin(), merged.end(), [&](const std::string& e) { return iequals(e, s); })) {
                    merged.push_back(s);
                }
            }
            strings = merged;
        }
        for (const auto& s : strings) {
            if (s.empty()) continue;
            const auto bytes = utf16_bytes(s);
            data.insert(data.end(), bytes.begin(), bytes.end());
        }
        data.push_back(0);
        data.push_back(0);
    } else if (type_bits == FLG_ADDREG_TYPE_DWORD) {
        type = REG_DWORD;
        const u32 v = values.empty() ? 0 : parse_number(values[0]).value_or(0);
        data = {static_cast<u8>(v), static_cast<u8>(v >> 8), static_cast<u8>(v >> 16), static_cast<u8>(v >> 24)};
    } else if (type_bits == FLG_ADDREG_TYPE_QWORD) {
        type = REG_QWORD;
        const u64 v = values.empty() ? 0 : parse_wide_number(values[0]).value_or(0);
        for (int i = 0; i < 8; ++i) data.push_back(static_cast<u8>(v >> (8 * i)));
    } else if (type_bits == FLG_ADDREG_TYPE_NONE || (type_bits & 0xFFFFu) == FLG_ADDREG_BINVALUETYPE) {
        // Binary and custom types: the high word is the REG_* type, the data
        // a comma-separated byte list (hex, per Learn's example).
        type = type_bits == FLG_ADDREG_TYPE_NONE ? REG_NONE
               : (type_bits >> 16) == 0         ? REG_BINARY
                                                : (type_bits >> 16);
        for (const auto& v : values) {
            const auto byte = parse_wide_number(v, 16);
            if (!byte || *byte > 0xFF) return error_codes::ERROR_INVALID_DATA;
            data.push_back(static_cast<u8>(*byte));
        }
    } else {
        char what[64];
        std::snprintf(what, sizeof what, "AddReg flags 0x%x", flags);
        log_unimplemented_once("setupapi!SetupInstallFromInfSection", what);
        return error_codes::ERROR_CALL_NOT_IMPLEMENTED;
    }
    return reg.reg_set_value_ex_w(key, value_name, type, data.data(), static_cast<u32>(data.size()));
}

[[nodiscard]] u32 apply_del_reg(RegistryThunks& reg, const InfFile& inf, const InfLine& line,
                                u64 relative_root, bool is64) {
    // reg-root,subkey[,value-entry-name][,flags][,value]
    const auto root = registry_root(inf.field(line, 1).value_or(""), relative_root);
    if (!root) return error_codes::ERROR_INVALID_DATA;
    const std::string subkey = inf.field(line, 2).value_or("");
    const std::string name = inf.field(line, 3).value_or("");
    const std::string flag_text = inf.field(line, 4).value_or("");
    const u32 flags = flag_text.empty() ? 0 : parse_number(flag_text).value_or(0xFFFFFFFFu);
    if (flags == 0xFFFFFFFFu) return error_codes::ERROR_INVALID_DATA;
    if (foreign_view(flags, is64)) {
        log_unimplemented_once("setupapi!SetupInstallFromInfSection", "DelReg on the other bitness's view");
        return error_codes::ERROR_CALL_NOT_IMPLEMENTED;
    }
    const u32 base = flags & ~(FLG_DELREG_32BITKEY | FLG_DELREG_64BITKEY);
    if (base == FLG_DELREG_MULTI_SZ_DELSTRING) {
        log_unimplemented_once("setupapi!SetupInstallFromInfSection", "DelReg FLG_DELREG_MULTI_SZ_DELSTRING");
        return error_codes::ERROR_CALL_NOT_IMPLEMENTED;
    }
    if (base != 0 && base != FLG_DELREG_KEYONLY_COMMON) return error_codes::ERROR_INVALID_DATA;
    if (base == 0 && !name.empty()) {
        (void)reg.reg_delete_key_value_w(*root, widen(subkey), widen(name));
        return error_codes::ERROR_SUCCESS;
    }
    // A key that is already absent is what the directive asks for.
    if (reg.reg_delete_tree(*root, widen(subkey)) == error_codes::ERROR_SUCCESS) {
        (void)reg.reg_delete_key(*root, widen(subkey));
    }
    return error_codes::ERROR_SUCCESS;
}

// ---- SP_INF_INFORMATION ----------------------------------------------------------
//
// Measured layout (setupapi_inf_probe*.expected; the same in A and W, both
// bitnesses): InfStyle, InfCount, then per INF a 24-byte header
//   u32 0 | FILETIME last write | u16 key count | u16 name bytes |
//   u32 pair bytes | u32 block bytes (header + name + pairs)
// followed by the UTF-16 path and the [Version] key\0value\0 pairs (value
// = first field, substituted).

[[nodiscard]] std::vector<u8> build_inf_information(const OpenInf& open) {
    const auto pairs = open.inf.version_pairs();
    const std::vector<u8> name = utf16_bytes(open.path);
    std::vector<u8> pair_bytes;
    for (const auto& [k, v] : pairs) {
        const auto kb = utf16_bytes(k), vb = utf16_bytes(v);
        pair_bytes.insert(pair_bytes.end(), kb.begin(), kb.end());
        pair_bytes.insert(pair_bytes.end(), vb.begin(), vb.end());
    }
    std::vector<u8> out;
    const auto put32 = [&](u32 v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i))); };
    const auto put16 = [&](u16 v) { out.push_back(static_cast<u8>(v)); out.push_back(static_cast<u8>(v >> 8)); };
    put32(open.inf.style());
    put32(1);
    put32(0);
    put32(static_cast<u32>(open.last_write));
    put32(static_cast<u32>(open.last_write >> 32));
    put16(static_cast<u16>(pairs.size()));
    put16(static_cast<u16>(name.size()));
    put32(static_cast<u32>(pair_bytes.size()));
    put32(static_cast<u32>(24 + name.size() + pair_bytes.size()));
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), pair_bytes.begin(), pair_bytes.end());
    return out;
}

struct InfInfoBlock {
    std::string path;
    std::vector<std::pair<std::string, std::string>> pairs;
};

/// Parse one INF's block back out of a guest SP_INF_INFORMATION.
[[nodiscard]] std::optional<InfInfoBlock> read_info_block(Marshaler& m, GuestAddr info, u32 index) {
    const auto count = m.read_u32(info + 4);
    if (count.is_err() || index >= count.value()) return std::nullopt;
    GuestAddr block = info + 8;
    for (u32 i = 0;; ++i) {
        const auto size = m.read_u32(block + 20);
        if (size.is_err() || size.value() < 24) return std::nullopt;
        if (i == index) break;
        block += size.value();
    }
    const auto name_bytes = m.read_u16(block + 14);
    const auto pair_bytes = m.read_u32(block + 16);
    const auto keys = m.read_u16(block + 12);
    if (name_bytes.is_err() || pair_bytes.is_err() || keys.is_err()) return std::nullopt;
    InfInfoBlock out;
    auto path = m.read_wide_string(block + 24);
    if (path.is_err()) return std::nullopt;
    out.path = path.value();
    GuestAddr at = block + 24 + name_bytes.value();
    for (u16 k = 0; k < keys.value(); ++k) {
        auto key = m.read_wide_string(at);
        if (key.is_err()) return std::nullopt;
        at += static_cast<GuestAddr>(utf8_to_utf16(key.value()).size() + 1) * 2;
        auto value = m.read_wide_string(at);
        if (value.is_err()) return std::nullopt;
        at += static_cast<GuestAddr>(utf8_to_utf16(value.value()).size() + 1) * 2;
        out.pairs.emplace_back(key.value(), value.value());
    }
    return out;
}

/// Hand @p text to a (Buffer, Size in chars, RequiredSize) triple. NULL
/// buffer with size 0 is the documented size query.
bool return_text(Marshaler& m, const std::string& text, bool wide, GuestAddr buffer, u32 size,
                 GuestAddr required, bool multi = false) {
    u32 units = text_units(text, wide);
    if (multi) units = static_cast<u32>(wide ? utf8_to_utf16(text).size() : utf8_to_acp(text).size()) + 1;
    if (required != 0) (void)m.write_u32(required, units);
    if (buffer == 0) {
        if (size != 0) {
            set_last_error(ERROR_INVALID_USER_BUFFER);
            m.set_return(0);
            return false;
        }
        m.set_return(1);
        return true;
    }
    if (size < units) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return false;
    }
    if (multi) {
        // key\0key\0...\0\0: write unit by unit.
        if (wide) {
            const auto w = utf8_to_utf16(text);
            for (usize i = 0; i < w.size(); ++i) (void)m.write_u16(buffer + 2 * i, w[i]);
            (void)m.write_u16(buffer + 2 * w.size(), 0);
        } else {
            const auto a = utf8_to_acp(text);
            for (usize i = 0; i < a.size(); ++i) (void)m.write_u8(buffer + i, static_cast<u8>(a[i]));
            (void)m.write_u8(buffer + a.size(), 0);
        }
    } else {
        (void)write_text(m, buffer, text, wide);
    }
    m.set_return(1);
    return true;
}

// ---- opening --------------------------------------------------------------------

struct OpenResult {
    std::shared_ptr<OpenInf> inf;
    u32 error = 0;
    u32 line = 0;
};

[[nodiscard]] std::vector<std::string> search_paths(const std::string& name, u32 search) {
    if (name.find_first_of("\\/") != std::string::npos || search == INFINFO_INF_NAME_IS_ABSOLUTE) {
        return {name};
    }
    const std::string inf_dir = std::string(kWindows) + "\\INF\\" + name;
    const std::string sys_dir = std::string(kWindows) + "\\system32\\" + name;
    if (search == INFINFO_REVERSE_DEFAULT_SEARCH) return {sys_dir, inf_dir};
    // DEFAULT_SEARCH; PATH_LIST_SEARCH reads DevicePath, whose value is
    // %SystemRoot%\inf on a default installation.
    return {inf_dir, sys_dir};
}

[[nodiscard]] OpenResult open_inf(capsule::Vfs& vfs, const std::string& name, u32 search, bool is64) {
    OpenResult result;
    for (const auto& path : search_paths(name, search)) {
        if (!vfs_is_file(vfs, path)) continue;
        std::vector<u8> bytes;
        if (!vfs_read_all(vfs, path, bytes)) {
            result.error = error_codes::ERROR_ACCESS_DENIED;
            return result;
        }
        auto parsed = InfFile::parse(bytes, acp_to_utf8);
        if (parsed.is_err()) {
            result.error = parsed.error().error;
            result.line = parsed.error().line;
            return result;
        }
        auto open = std::make_shared<OpenInf>();
        open->inf = std::move(parsed).value();
        open->path = path;
        // %01% in an INF's text is the directory the INF is in (probe).
        open->inf.set_dirid_resolver([dir = parent_dir(path), is64](i64 id) { return dirid_path(id, dir, is64); });
        if (const auto st = vfs.stat(path); st.is_ok()) {
            open->last_write = st.value().last_write_time;
        }
        result.inf = std::move(open);
        return result;
    }
    result.error = error_codes::ERROR_FILE_NOT_FOUND;
    return result;
}

[[nodiscard]] std::shared_ptr<OpenInf> find_inf(const StatePtr& state, u64 handle) {
    std::lock_guard lock(state->mutex);
    const auto it = state->infs.find(handle);
    return it == state->infs.end() ? nullptr : it->second;
}


// ---- INFCONTEXT ---------------------------------------------------------------
//
// INFCONTEXT { PVOID Inf; PVOID CurrentInf; UINT Section; UINT Line; }
// (SetupAPI.h; 16 bytes x86, 24 x64, probe ctx-size). Section is this
// engine's section index, Line the 0-based line in it (probe: the first
// line is Line 0).

struct LineRef {
    std::shared_ptr<OpenInf> open;
    u64 hinf = 0;
    u32 section = 0;
    u32 line = 0;
    [[nodiscard]] const InfLine& get() const { return open->inf.sections()[section].lines[line]; }
};

[[nodiscard]] std::optional<u32> section_index(const InfFile& inf, std::string_view name) {
    const auto& sections = inf.sections();
    for (usize i = 0; i < sections.size(); ++i) {
        if (iequals(sections[i].name, name)) return static_cast<u32>(i);
    }
    return std::nullopt;
}

[[nodiscard]] bool write_context(Marshaler& m, GuestAddr at, const LineRef& ref) {
    const bool is64 = m.cpu_is_64bit();
    const u32 p = is64 ? 8 : 4;
    return write_ptr_field(m, at, ref.hinf, is64) && write_ptr_field(m, at + p, ref.hinf, is64) &&
           m.write_u32(at + 2 * p, ref.section).is_ok() && m.write_u32(at + 2 * p + 4, ref.line).is_ok();
}

[[nodiscard]] std::optional<LineRef> read_context(Marshaler& m, const StatePtr& state, GuestAddr at) {
    if (at == 0) return std::nullopt;
    const bool is64 = m.cpu_is_64bit();
    const u32 p = is64 ? 8 : 4;
    LineRef ref;
    ref.hinf = read_ptr_field(m, at + p, is64);
    ref.open = find_inf(state, ref.hinf);
    const auto section = m.read_u32(at + 2 * p);
    const auto line = m.read_u32(at + 2 * p + 4);
    if (!ref.open || section.is_err() || line.is_err()) return std::nullopt;
    ref.section = section.value();
    ref.line = line.value();
    const auto& sections = ref.open->inf.sections();
    if (ref.section >= sections.size() || ref.line >= sections[ref.section].lines.size()) return std::nullopt;
    return ref;
}

/// First line at or after @p from in @p section whose key is @p key (any
/// line when @p key is null).
[[nodiscard]] std::optional<u32> find_line(const InfFile& inf, u32 section, u32 from,
                                           const std::optional<std::string>& key) {
    const auto& lines = inf.sections()[section].lines;
    for (u32 i = from; i < lines.size(); ++i) {
        if (!key) return i;
        if (lines[i].has_key && iequals(inf.key(lines[i]), *key)) return i;
    }
    return std::nullopt;
}

/// SetupGetLineText: the substituted fields after the key, joined by ','
/// with no quotes or blanks; empty fields stay (probe: "Probe Disk,probe.cab,,").
[[nodiscard]] std::string line_text(const InfFile& inf, const InfLine& line) {
    std::string text;
    for (usize f = 1; f <= inf.field_count(line); ++f) {
        if (f != 1) text.push_back(',');
        text += *inf.field(line, f);
    }
    return text;
}

/// SetupGetIntField: 0x-prefixed hex or decimal (a leading '0' is not octal:
/// probe "077" is 77), optionally negative; an empty field is 0.
[[nodiscard]] std::optional<i32> int_field(std::string_view text) {
    if (text.empty()) return 0;
    bool negative = false;
    if (text.front() == '-' || text.front() == '+') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    int radix = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        radix = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) return std::nullopt;
    u64 value = 0;
    for (char c : text) {
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (radix == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (radix == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return std::nullopt;
        value = value * static_cast<u64>(radix) + static_cast<u64>(digit);
        if (value > 0xFFFFFFFFull) return std::nullopt;
    }
    const u32 v = static_cast<u32>(value);
    return negative ? -static_cast<i32>(v) : static_cast<i32>(v);
}

}  // namespace

static void add_inf_line_exports(DllExports& exports, const StatePtr& state) {
    // BOOL SetupFindFirstLine(HINF, PCTSTR Section, PCTSTR Key, PINFCONTEXT)
    const auto find_first = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            const u64 hinf = m.arg(0);
            const auto open = find_inf(state, hinf);
            const auto section = read_text(m, m.arg_ptr(1), wide);
            const auto key = read_text(m, m.arg_ptr(2), wide);
            const GuestAddr context = m.arg_ptr(3);
            if (!open) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
            if (!section || context == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
            const auto index = section_index(open->inf, *section);
            const auto line = index ? find_line(open->inf, *index, 0, key) : std::nullopt;
            if (!line) { set_last_error(ERROR_LINE_NOT_FOUND); m.set_return(0); return; }
            (void)write_context(m, context, LineRef{open, hinf, *index, *line});
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        };
    };
    exports.entries.push_back({"SetupFindFirstLineA", 0, find_first(false), 16});
    exports.entries.push_back({"SetupFindFirstLineW", 0, find_first(true), 16});

    // BOOL SetupFindNextLine(PINFCONTEXT ContextIn, PINFCONTEXT ContextOut)
    // BOOL SetupFindNextMatchLine(PINFCONTEXT ContextIn, PCTSTR Key, PINFCONTEXT ContextOut)
    const auto find_next = [state](bool match, bool wide) {
        return [state, match, wide](Marshaler& m) {
            auto ref = read_context(m, state, m.arg_ptr(0));
            const std::optional<std::string> key = match ? read_text(m, m.arg_ptr(1), wide) : std::nullopt;
            const GuestAddr out = m.arg_ptr(match ? 2 : 1);
            if (!ref || out == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
            const auto line = find_line(ref->open->inf, ref->section, ref->line + 1, key);
            if (!line) { set_last_error(ERROR_LINE_NOT_FOUND); m.set_return(0); return; }
            ref->line = *line;
            (void)write_context(m, out, *ref);
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        };
    };
    exports.entries.push_back({"SetupFindNextLine", 0, find_next(false, false), 8});
    exports.entries.push_back({"SetupFindNextMatchLineA", 0, find_next(true, false), 12});
    exports.entries.push_back({"SetupFindNextMatchLineW", 0, find_next(true, true), 12});

    // LONG SetupGetLineCount(HINF, PCTSTR Section)
    const auto line_count = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            const auto open = find_inf(state, m.arg(0));
            const auto section = read_text(m, m.arg_ptr(1), wide);
            const auto index = open && section ? section_index(open->inf, *section) : std::nullopt;
            if (!index) {
                set_last_error(open ? ERROR_SECTION_NOT_FOUND : error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(static_cast<u32>(open->inf.sections()[*index].lines.size()));
        };
    };
    exports.entries.push_back({"SetupGetLineCountA", 0, line_count(false), 8});
    exports.entries.push_back({"SetupGetLineCountW", 0, line_count(true), 8});

    // BOOL SetupGetLineByIndex(HINF, PCTSTR Section, DWORD Index, PINFCONTEXT)
    const auto by_index = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            const u64 hinf = m.arg(0);
            const auto open = find_inf(state, hinf);
            const auto section = read_text(m, m.arg_ptr(1), wide);
            const u32 index = static_cast<u32>(m.arg(2));
            const GuestAddr context = m.arg_ptr(3);
            if (!open) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
            const auto s = section ? section_index(open->inf, *section) : std::nullopt;
            if (!s || index >= open->inf.sections()[*s].lines.size() || context == 0) {
                set_last_error(ERROR_LINE_NOT_FOUND);
                m.set_return(0);
                return;
            }
            (void)write_context(m, context, LineRef{open, hinf, *s, index});
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        };
    };
    exports.entries.push_back({"SetupGetLineByIndexA", 0, by_index(false), 16});
    exports.entries.push_back({"SetupGetLineByIndexW", 0, by_index(true), 16});

    // BOOL SetupGetLineText(PINFCONTEXT Context, HINF, PCTSTR Section, PCTSTR Key,
    //     PTSTR ReturnBuffer, DWORD ReturnBufferSize, PDWORD RequiredSize)
    const auto get_line_text = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            std::optional<LineRef> ref;
            if (m.arg_ptr(0) != 0) {
                ref = read_context(m, state, m.arg_ptr(0));
                if (!ref) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
            } else {
                const u64 hinf = m.arg(1);
                const auto open = find_inf(state, hinf);
                const auto section = read_text(m, m.arg_ptr(2), wide);
                const auto key = read_text(m, m.arg_ptr(3), wide);
                if (!open) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
                const auto s = section ? section_index(open->inf, *section) : std::nullopt;
                const auto line = s ? find_line(open->inf, *s, 0, key) : std::nullopt;
                if (!line) { set_last_error(ERROR_LINE_NOT_FOUND); m.set_return(0); return; }
                ref = LineRef{open, hinf, *s, *line};
            }
            if (return_text(m, line_text(ref->open->inf, ref->get()), wide, m.arg_ptr(4),
                            static_cast<u32>(m.arg(5)), m.arg_ptr(6))) {
                set_last_error(error_codes::ERROR_SUCCESS);
            }
        };
    };
    exports.entries.push_back({"SetupGetLineTextA", 0, get_line_text(false), 28});
    exports.entries.push_back({"SetupGetLineTextW", 0, get_line_text(true), 28});

    // DWORD SetupGetFieldCount(PINFCONTEXT Context)
    exports.entries.push_back({"SetupGetFieldCount", 0, [state](Marshaler& m) {
        const auto ref = read_context(m, state, m.arg_ptr(0));
        if (!ref) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(static_cast<u32>(ref->open->inf.field_count(ref->get())));
    }, 4});

    // BOOL SetupGetStringField(PINFCONTEXT, DWORD FieldIndex, PTSTR ReturnBuffer,
    //     DWORD ReturnBufferSize, PDWORD RequiredSize)
    const auto string_field = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            const auto ref = read_context(m, state, m.arg_ptr(0));
            const auto value = ref ? ref->open->inf.field(ref->get(), static_cast<u32>(m.arg(1))) : std::nullopt;
            if (!value) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
            if (return_text(m, *value, wide, m.arg_ptr(2), static_cast<u32>(m.arg(3)), m.arg_ptr(4))) {
                set_last_error(error_codes::ERROR_SUCCESS);
            }
        };
    };
    exports.entries.push_back({"SetupGetStringFieldA", 0, string_field(false), 20});
    exports.entries.push_back({"SetupGetStringFieldW", 0, string_field(true), 20});

    // BOOL SetupGetIntField(PINFCONTEXT, DWORD FieldIndex, PINT IntegerValue)
    exports.entries.push_back({"SetupGetIntField", 0, [state](Marshaler& m) {
        const auto ref = read_context(m, state, m.arg_ptr(0));
        const auto value = ref ? ref->open->inf.field(ref->get(), static_cast<u32>(m.arg(1))) : std::nullopt;
        const GuestAddr out = m.arg_ptr(2);
        if (!value || out == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        const auto number = int_field(*value);
        if (!number) { set_last_error(error_codes::ERROR_INVALID_DATA); m.set_return(0); return; }
        (void)m.write_u32(out, static_cast<u32>(*number));
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(1);
    }, 12});
}

void setupapi_internal::add_inf_exports(DllExports& exports, const StatePtr& state) {
    add_inf_line_exports(exports, state);
    // HINF SetupOpenInfFile(PCTSTR FileName, PCTSTR InfClass, DWORD InfStyle, PUINT ErrorLine)
    const auto open_file = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            const auto name = read_text(m, m.arg_ptr(0), wide);
            const auto cls = read_text(m, m.arg_ptr(1), wide);
            const u32 style = static_cast<u32>(m.arg(2));
            const GuestAddr error_line = m.arg_ptr(3);
            if (error_line != 0) (void)m.write_u32(error_line, 0);
            const auto fail = [&](u32 error) {
                set_last_error(error);
                m.set_return(invalid_handle_value(m));
            };
            if (!name || state->vfs == nullptr) return fail(error_codes::ERROR_INVALID_PARAMETER);
            OpenResult opened = open_inf(*state->vfs, *name, INFINFO_DEFAULT_SEARCH, m.cpu_is_64bit());
            if (!opened.inf) {
                if (error_line != 0) (void)m.write_u32(error_line, opened.line);
                return fail(opened.error);
            }
            if ((style & opened.inf->inf.style()) == 0) return fail(ERROR_WRONG_INF_STYLE);
            if (cls && opened.inf->inf.style() == setupapi_inf::INF_STYLE_WIN4) {
                const auto declared = opened.inf->inf.value("Version", "Class");
                if (!declared || !iequals(*declared, *cls)) return fail(ERROR_CLASS_MISMATCH);
            }
            u64 handle;
            {
                std::lock_guard lock(state->mutex);
                handle = state->next_inf;
                state->next_inf += 4;
                state->infs.emplace(handle, opened.inf);
            }
            VITREO_TRACE("[setupapi] SetupOpenInfFile '%s' -> 0x%llx\n", name->c_str(),
                         static_cast<unsigned long long>(handle));
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(handle);
        };
    };
    exports.entries.push_back({"SetupOpenInfFileA", 0, open_file(false), 16});
    exports.entries.push_back({"SetupOpenInfFileW", 0, open_file(true), 16});

    // VOID SetupCloseInfFile(HINF InfHandle)
    exports.entries.push_back({"SetupCloseInfFile", 0, [state](Marshaler& m) {
        const u64 handle = m.arg(0);
        std::lock_guard lock(state->mutex);
        state->infs.erase(handle);
    }, 4});

    // BOOL SetupGetInfInformation(LPCVOID InfSpec, DWORD SearchControl,
    //     PSP_INF_INFORMATION ReturnBuffer, DWORD ReturnBufferSize, PDWORD RequiredSize)
    const auto get_info = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            const GuestAddr spec = m.arg_ptr(0);
            const u32 search = static_cast<u32>(m.arg(1));
            const GuestAddr buffer = m.arg_ptr(2);
            const u32 size = static_cast<u32>(m.arg(3));
            const GuestAddr required = m.arg_ptr(4);
            std::shared_ptr<OpenInf> open;
            if (search == INFINFO_INF_SPEC_IS_HINF) {
                open = find_inf(state, spec);
                if (!open) {
                    set_last_error(error_codes::ERROR_INVALID_HANDLE);
                    m.set_return(0);
                    return;
                }
            } else if (search >= INFINFO_INF_NAME_IS_ABSOLUTE && search <= INFINFO_INF_PATH_LIST_SEARCH) {
                const auto name = read_text(m, spec, wide);
                if (!name || state->vfs == nullptr) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                OpenResult opened = open_inf(*state->vfs, *name, search, m.cpu_is_64bit());
                if (!opened.inf) {
                    set_last_error(opened.error);
                    m.set_return(0);
                    return;
                }
                open = opened.inf;
                // Measured: the by-name forms leave the last error 0 on
                // success, the HINF form leaves it untouched.
                set_last_error(error_codes::ERROR_SUCCESS);
            } else {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const std::vector<u8> info = build_inf_information(*open);
            if (required != 0) (void)m.write_u32(required, static_cast<u32>(info.size()));
            if (buffer == 0) {
                m.set_return(1);
                return;
            }
            if (size < info.size()) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            for (usize i = 0; i < info.size(); ++i) (void)m.write_u8(buffer + i, info[i]);
            m.set_return(1);
        };
    };
    exports.entries.push_back({"SetupGetInfInformationA", 0, get_info(false), 20});
    exports.entries.push_back({"SetupGetInfInformationW", 0, get_info(true), 20});

    // BOOL SetupQueryInfVersionInformation(PSP_INF_INFORMATION, UINT InfIndex,
    //     PCTSTR Key, PTSTR ReturnBuffer, DWORD ReturnBufferSize, PDWORD RequiredSize)
    const auto query_version = [](bool wide) {
        return [wide](Marshaler& m) {
            const auto block = read_info_block(m, m.arg_ptr(0), static_cast<u32>(m.arg(1)));
            const GuestAddr key_ptr = m.arg_ptr(2);
            if (!block) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (key_ptr == 0) {
                // Every key, each NUL-terminated, then a final NUL.
                std::string all;
                for (const auto& [k, v] : block->pairs) {
                    all += k;
                    all.push_back('\0');
                }
                return_text(m, all, wide, m.arg_ptr(3), static_cast<u32>(m.arg(4)), m.arg_ptr(5), true);
                return;
            }
            const auto key = read_text(m, key_ptr, wide);
            for (const auto& [k, v] : block->pairs) {
                if (key && iequals(k, *key)) {
                    // Measured: success leaves the last error 0.
                    if (return_text(m, v, wide, m.arg_ptr(3), static_cast<u32>(m.arg(4)), m.arg_ptr(5))) {
                        set_last_error(error_codes::ERROR_SUCCESS);
                    }
                    return;
                }
            }
            set_last_error(error_codes::ERROR_INVALID_DATA);
            m.set_return(0);
        };
    };
    exports.entries.push_back({"SetupQueryInfVersionInformationA", 0, query_version(false), 24});
    exports.entries.push_back({"SetupQueryInfVersionInformationW", 0, query_version(true), 24});

    // BOOL SetupQueryInfFileInformation(PSP_INF_INFORMATION, UINT InfIndex,
    //     PTSTR ReturnBuffer, DWORD ReturnBufferSize, PDWORD RequiredSize)
    const auto query_file = [](bool wide) {
        return [wide](Marshaler& m) {
            const auto block = read_info_block(m, m.arg_ptr(0), static_cast<u32>(m.arg(1)));
            if (!block) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            return_text(m, block->path, wide, m.arg_ptr(2), static_cast<u32>(m.arg(3)), m.arg_ptr(4));
        };
    };
    exports.entries.push_back({"SetupQueryInfFileInformationA", 0, query_file(false), 20});
    exports.entries.push_back({"SetupQueryInfFileInformationW", 0, query_file(true), 20});

    // BOOL SetupInstallFromInfSection(HWND Owner, HINF InfHandle, PCTSTR SectionName,
    //     UINT Flags, HKEY RelativeKeyRoot, PCTSTR SourceRootPath, UINT CopyFlags,
    //     PSP_FILE_CALLBACK MsgHandler, PVOID Context, HDEVINFO DeviceInfoSet,
    //     PSP_DEVINFO_DATA DeviceInfoData)
    const auto install = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            const u64 owner = m.arg(0);
            const auto open = find_inf(state, m.arg(1));
            const auto section_name = read_text(m, m.arg_ptr(2), wide);
            const u32 flags = static_cast<u32>(m.arg(3));
            const u64 relative_root = m.arg(4);
            const auto source_root = read_text(m, m.arg_ptr(5), wide);
            const u32 copy_style = static_cast<u32>(m.arg(6));
            const GuestAddr handler = m.arg_ptr(7);
            const GuestAddr context = m.arg_ptr(8);
            const u64 device_set = m.arg(9);
            const auto fail = [&](u32 error) {
                set_last_error(error);
                m.set_return(0);
            };
            if (!open) return fail(error_codes::ERROR_INVALID_HANDLE);
            if (!section_name) return fail(error_codes::ERROR_INVALID_PARAMETER);
            if (device_set != 0) {
                log_unimplemented_once("setupapi!SetupInstallFromInfSection", "DeviceInfoSet installs");
                return fail(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            }
            const auto* section = open->inf.section(*section_name);
            if (section == nullptr) {
                // Measured: a section the INF lacks installs nothing, successfully.
                set_last_error(error_codes::ERROR_SUCCESS);
                m.set_return(1);
                return;
            }
            // Directives this engine does not carry out fail instead of being
            // skipped when the caller asked for their kind.
            struct Unsupported { u32 flag; const char* directive; };
            static constexpr Unsupported kUnsupported[] = {
                {SPINST_INIFILES, "UpdateInis"}, {SPINST_INIFILES, "UpdateIniFields"},
                {SPINST_INIFILES, "Ini2Reg"}, {SPINST_INI2REG, "Ini2Reg"},
                {SPINST_BITREG, "BitReg"}, {SPINST_REGSVR, "RegisterDlls"},
                {SPINST_UNREGSVR, "UnregisterDlls"}, {SPINST_PROFILEITEMS, "ProfileItems"},
                {SPINST_LOGCONFIG, "LogConfig"}, {SPINST_COPYINF, "CopyINF"},
                {SPINST_PROPERTIES, "AddProperty"}, {SPINST_PROPERTIES, "DelProperty"},
                {0xFFFFFFFFu, "Include"}, {0xFFFFFFFFu, "Needs"},
            };
            for (const auto& line : section->lines) {
                const std::string key = open->inf.key(line);
                for (const auto& u : kUnsupported) {
                    if ((flags & u.flag) != 0 && iequals(key, u.directive)) {
                        log_unimplemented_once("setupapi!SetupInstallFromInfSection", u.directive);
                        return fail(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                    }
                }
            }
            if ((flags & SPINST_FILES) != 0) {
                if ((copy_style & ~kSupportedCopyStyle) != 0) {
                    char what[64];
                    std::snprintf(what, sizeof what, "CopyFlags 0x%x", copy_style);
                    log_unimplemented_once("setupapi!SetupInstallFromInfSection", what);
                    return fail(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                }
                const std::string root = source_root && !source_root->empty()
                                             ? *source_root
                                             : parent_dir(open->path);
                InfInstaller installer(*open, root, copy_style, m.cpu_is_64bit());
                Queue q;
                if (auto e = installer.queue_files(*section, q)) return fail(e->error);
                if (!q.deletes.empty() || !q.renames.empty() || !q.copies.empty()) {
                    Notifier notify(m, state, handler, context, wide);
                    if (!notify.usable() || state->vfs == nullptr) {
                        return fail(error_codes::ERROR_INVALID_PARAMETER);
                    }
                    if (!commit_queue(m, state, notify, owner, q)) {
                        m.set_return(0);
                        return;
                    }
                }
            }
            if ((flags & SPINST_REGISTRY) != 0) {
                for (const auto& line : section->lines) {
                    const std::string key = open->inf.key(line);
                    const bool add = iequals(key, "AddReg");
                    if (!add && !iequals(key, "DelReg")) continue;
                    if (state->registry == nullptr) return fail(error_codes::ERROR_INVALID_PARAMETER);
                    for (usize f = 1; f <= open->inf.field_count(line); ++f) {
                        const auto* reg_section = open->inf.section(*open->inf.field(line, f));
                        if (reg_section == nullptr) return fail(ERROR_SECTION_NOT_FOUND);
                        for (const auto& entry : reg_section->lines) {
                            const u32 r = add ? apply_add_reg(*state->registry, open->inf, entry,
                                                              relative_root, m.cpu_is_64bit())
                                              : apply_del_reg(*state->registry, open->inf, entry,
                                                              relative_root, m.cpu_is_64bit());
                            if (r != error_codes::ERROR_SUCCESS) return fail(r);
                        }
                    }
                }
            }
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        };
    };
    exports.entries.push_back({"SetupInstallFromInfSectionA", 0, install(false), 44});
    exports.entries.push_back({"SetupInstallFromInfSectionW", 0, install(true), 44});

    // PVOID SetupInitDefaultQueueCallback(HWND OwnerWindow)
    // PVOID SetupInitDefaultQueueCallbackEx(HWND, HWND AlternateProgressWindow,
    //     UINT ProgressMessage, DWORD Reserved1, PVOID Reserved2)
    const auto init_default = [state](bool ex) {
        return [state, ex](Marshaler& m) {
            DefaultQueueContext ctx;
            ctx.owner = m.arg(0);
            ctx.progress_window = ex ? m.arg(1) : 0;
            const bool no_progress = ex && (ctx.progress_window == invalid_handle_value(m) ||
                                            static_cast<u32>(ctx.progress_window) == 0xFFFFFFFFu);
            ctx.wants_ui = !no_progress;
            if (ctx.wants_ui) {
                log_unimplemented_once("setupapi!SetupInitDefaultQueueCallback",
                                       "progress and prompt dialogs (behaves as no-UI)");
            }
            std::lock_guard lock(state->mutex);
            const u64 handle = state->next_default_context;
            state->next_default_context += 4;
            state->default_contexts.emplace(handle, ctx);
            m.set_return(handle);
        };
    };
    exports.entries.push_back({"SetupInitDefaultQueueCallback", 0, init_default(false), 4});
    exports.entries.push_back({"SetupInitDefaultQueueCallbackEx", 0, init_default(true), 20});

    // VOID SetupTermDefaultQueueCallback(PVOID Context)
    exports.entries.push_back({"SetupTermDefaultQueueCallback", 0, [state](Marshaler& m) {
        const u64 handle = m.arg(0);
        std::lock_guard lock(state->mutex);
        state->default_contexts.erase(handle);
    }, 4});

    // UINT SetupDefaultQueueCallback(PVOID Context, UINT Notification,
    //     UINT_PTR Param1, UINT_PTR Param2)
    //
    // Measured answers of a no-UI context: TRUE for the queue and sub-queue
    // notifications and the END* ones, FILEOP_DOIT for STARTCOPY and
    // NEEDMEDIA when the medium's file or tag is present, FILEOP_ABORT for a
    // NEEDMEDIA it cannot satisfy (the commit then fails with
    // ERROR_FILE_NOT_FOUND), 0 for FILEOPDELAYED, and 0 with
    // ERROR_INVALID_PARAMETER for a NULL context.
    const auto default_callback = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            const u64 context = m.arg(0);
            const u32 note = static_cast<u32>(m.arg(1));
            const GuestAddr p1 = m.arg_ptr(2);
            bool known;
            {
                std::lock_guard lock(state->mutex);
                known = state->default_contexts.count(context) != 0;
            }
            if (!known) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const bool is64 = m.cpu_is_64bit();
            const u32 p = is64 ? 8 : 4;
            switch (note) {
                case SPFILENOTIFY_STARTQUEUE:
                case SPFILENOTIFY_ENDQUEUE:
                case SPFILENOTIFY_STARTSUBQUEUE:
                case SPFILENOTIFY_ENDSUBQUEUE:
                case SPFILENOTIFY_ENDCOPY:
                case SPFILENOTIFY_ENDDELETE:
                case SPFILENOTIFY_ENDRENAME:
                    m.set_return(1);
                    return;
                case SPFILENOTIFY_STARTCOPY:
                case SPFILENOTIFY_STARTDELETE:
                case SPFILENOTIFY_STARTRENAME:
                    m.set_return(FILEOP_DOIT);
                    return;
                case SPFILENOTIFY_FILEOPDELAYED:
                    m.set_return(0);
                    return;
                case SPFILENOTIFY_NEEDMEDIA: {
                    const auto tag = read_text(m, static_cast<GuestAddr>(read_ptr_field(m, p1 + p, is64)), wide);
                    const auto path = read_text(m, static_cast<GuestAddr>(read_ptr_field(m, p1 + 3 * p, is64)), wide);
                    const auto file = read_text(m, static_cast<GuestAddr>(read_ptr_field(m, p1 + 4 * p, is64)), wide);
                    const bool present =
                        state->vfs != nullptr && path &&
                        ((file && vfs_is_file(*state->vfs, join_win_path(*path, *file))) ||
                         (tag && !tag->empty() && vfs_is_file(*state->vfs, join_win_path(*path, *tag))));
                    if (present) {
                        m.set_return(FILEOP_DOIT);
                        return;
                    }
                    // No prompt is possible: the medium is missing.
                    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
                    m.set_return(FILEOP_ABORT);
                    return;
                }
                case SPFILENOTIFY_COPYERROR:
                case SPFILENOTIFY_DELETEERROR:
                case SPFILENOTIFY_RENAMEERROR: {
                    // Without a prompt the operation cannot be retried or
                    // redirected; the error stands.
                    const auto error = m.read_u32(p1 + 2 * p);
                    set_last_error(error.is_ok() ? error.value() : 0);
                    m.set_return(FILEOP_ABORT);
                    return;
                }
                default:
                    if ((note & (SPFILENOTIFY_LANGMISMATCH | SPFILENOTIFY_TARGETEXISTS |
                                 SPFILENOTIFY_TARGETNEWER)) != 0) {
                        // Learn: TRUE overwrites, FALSE skips; with no prompt
                        // the existing file is kept.
                        m.set_return(0);
                        return;
                    }
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
            }
        };
    };
    exports.entries.push_back({"SetupDefaultQueueCallbackA", 0, default_callback(false), 16});
    exports.entries.push_back({"SetupDefaultQueueCallbackW", 0, default_callback(true), 16});
    // The undecorated name is the ANSI routine (SetupAPI.lib exports both).
    exports.entries.push_back({"SetupDefaultQueueCallback", 0, default_callback(false), 16});

    // BOOL SetupIterateCabinet(PCTSTR CabinetFile, DWORD Reserved,
    //     PSP_FILE_CALLBACK MsgHandler, PVOID Context)
    const auto iterate = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            const auto cab_path = read_text(m, m.arg_ptr(0), wide);
            const GuestAddr handler = m.arg_ptr(2);
            const GuestAddr context = m.arg_ptr(3);
            const auto fail = [&](u32 error) {
                set_last_error(error);
                m.set_return(0);
            };
            Notifier notify(m, state, handler, context, wide);
            if (!cab_path || state->vfs == nullptr || !notify.usable()) {
                return fail(error_codes::ERROR_INVALID_PARAMETER);
            }
            capsule::Vfs& vfs = *state->vfs;
            if (!vfs_is_file(vfs, *cab_path)) return fail(error_codes::ERROR_FILE_NOT_FOUND);
            std::vector<u8> image;
            if (!vfs_read_all(vfs, *cab_path, image)) return fail(error_codes::ERROR_ACCESS_DENIED);
            auto parsed = compression::cabinet::Cabinet::parse(image);
            if (parsed.is_err()) return fail(error_codes::ERROR_INVALID_DATA);
            const auto& cab = parsed.value();
            Scratch scratch(m, state);
            const bool is64 = m.cpu_is_64bit();
            const u32 p = is64 ? 8 : 4;

            // CABINETINFO: CabinetPath (directory, with its separator),
            // CabinetFile / DiskName (the next cabinet in the set, empty for
            // the last), SetId, CabinetNumber.
            const GuestAddr ci = scratch.alloc(3 * p + 4);
            if (ci == 0) return fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            const std::string dir = parent_dir(*cab_path) + "\\";
            (void)write_ptr_field(m, ci, scratch.text(dir, wide), is64);
            (void)write_ptr_field(m, ci + p, scratch.text(cab.header().next_cabinet, wide), is64);
            (void)write_ptr_field(m, ci + 2 * p, scratch.text(cab.header().next_disk, wide), is64);
            (void)m.write_u16(ci + 3 * p, cab.header().set_id);
            (void)m.write_u16(ci + 3 * p + 2, cab.header().cabinet_index);
            if (const u32 r = notify(SPFILENOTIFY_CABINETINFO, ci, 0); r != error_codes::ERROR_SUCCESS) {
                return fail(r);
            }
            std::map<usize, std::vector<u8>> folders;
            const GuestAddr cab_name = scratch.text(*cab_path, wide);
            for (const auto& file : cab.files()) {
                if (file.spans_cabinets()) {
                    log_unimplemented_once("setupapi!SetupIterateCabinet",
                                           "files continued across cabinets (NEEDNEWCABINET)");
                    return fail(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                }
                // FILE_IN_CABINET_INFO: NameInCabinet, FileSize, Win32Error,
                // DosDate, DosTime, DosAttribs, FullTargetName[MAX_PATH].
                const u32 name_at = p + 14;
                const u32 record_size = name_at + kMaxPath * (wide ? 2u : 1u);
                const GuestAddr fi = scratch.alloc(record_size);
                if (fi == 0) return fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                (void)write_ptr_field(m, fi, scratch.text(file.name, wide), is64);
                (void)m.write_u32(fi + p, file.size);
                (void)m.write_u32(fi + p + 4, 0);
                (void)m.write_u16(fi + p + 8, file.date);
                (void)m.write_u16(fi + p + 10, file.time);
                (void)m.write_u16(fi + p + 12, file.attributes);
                for (u32 i = 0; i < kMaxPath * (wide ? 2u : 1u); ++i) (void)m.write_u8(fi + name_at + i, 0);
                const u32 answer = notify(SPFILENOTIFY_FILEINCABINET, fi, cab_name);
                if (answer == FILEOP_ABORT) break;  // measured: TRUE, last error 0
                if (answer == FILEOP_SKIP) continue;
                if (answer != FILEOP_DOIT) return fail(error_codes::ERROR_INVALID_PARAMETER);
                const auto target = read_text(m, fi + name_at, wide).value_or("");
                u32 error = error_codes::ERROR_SUCCESS;
                auto& data = folders[file.folder_index];
                if (data.empty()) {
                    auto folder = cab.extract_folder(file.folder_index);
                    if (folder.is_ok()) data = std::move(folder).value();
                }
                auto bytes = compression::cabinet::Cabinet::slice_file(file, data);
                if (bytes.is_err()) {
                    error = error_codes::ERROR_INVALID_DATA;
                } else if (target.empty() || !vfs_write_all(vfs, target, bytes.value())) {
                    error = error_codes::ERROR_PATH_NOT_FOUND;
                }
                const GuestAddr fp = make_filepaths(m, scratch, wide, target, *cab_path, error, 0);
                if (const u32 r = notify(SPFILENOTIFY_FILEEXTRACTED, fp, 0); r != error_codes::ERROR_SUCCESS) {
                    return fail(r);
                }
            }
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        };
    };
    exports.entries.push_back({"SetupIterateCabinetA", 0, iterate(false), 16});
    exports.entries.push_back({"SetupIterateCabinetW", 0, iterate(true), 16});
}

}  // namespace vitreo::winapi
