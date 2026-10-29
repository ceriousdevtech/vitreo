// SPDX-License-Identifier: LGPL-2.1-only

/// @file oleaut32_typelib.cpp
/// @brief Type library loading, registration, and the ITypeLib/ITypeInfo
///        objects handed back to the guest.
///
/// A type library describes an automation interface — its methods, their
/// parameter types, and the GUIDs identifying it. Guests load one to discover
/// what they can call through IDispatch, and installers register one so other
/// programs can find it later.
///
/// Two things make this its own domain. Registration writes to the capsule
/// registry rather than to memory, so a library registered by an installer is
/// still there for a later run. And the objects returned are real COM objects
/// with guest-visible vtables and reference counts, so their lifetime is the
/// guest's to manage, not this module's.
///
/// Split out of oleaut32.cpp, which was 4,316 lines.

#include "oleaut32.hpp"
#include "typeinfo_layout.generated.hpp"
#include "winapi/com/oleaut32/oleaut32_internal.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Type libraries
// =============================================================================

namespace {

// HRESULTs the type-library APIs are defined to return.
constexpr u32 E_INVALIDARG            = 0x80070057u;
constexpr u32 E_NOINTERFACE           = 0x80004002u;
constexpr u32 E_OUTOFMEMORY           = 0x8007000Eu;
constexpr u32 TYPE_E_CANTLOADLIBRARY  = 0x80029C4Au;
constexpr u32 TYPE_E_ELEMENTNOTFOUND  = 0x8002802Bu;
constexpr u32 TYPE_E_REGISTRYACCESS   = 0x8002801Cu;
constexpr u32 TYPE_E_LIBNOTREGISTERED_HR = 0x8002801Du;

/// Number of slots in the ITypeLib vtable (IUnknown's three plus ten of its
/// own). The object we hand the guest must have every one of them filled in:
/// a caller is entitled to invoke any slot, and a hole would be a wild jump.
constexpr usize TYPELIB_METHOD_COUNT = 13;
/// ITypeInfo: IUnknown (3) + 19 of its own.
constexpr usize TYPEINFO_METHOD_COUNT = 22;

/// TLIBATTR is { GUID; LCID; SYSKIND; WORD; WORD; WORD; } padded to 4 bytes.
constexpr u32 TLIBATTR_SIZE = 32;

/// IID_IUnknown and IID_ITypeLib, as the 16 bytes they occupy in memory.
constexpr std::array<u8, 16> IID_IUNKNOWN_BYTES{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr std::array<u8, 16> IID_ITYPELIB_BYTES{
    0x02, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr std::array<u8, 16> IID_ITYPEINFO_BYTES{
    0x01, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
/// IID_ITypeInfo2. QueryInterface after GetTypeInfoOfGuid returns the same
/// ITypeInfo object. The additional ITypeInfo2 methods are not implemented.
constexpr std::array<u8, 16> IID_ITYPEINFO2_BYTES{
    0x12, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

/// Read the whole of a guest file through the capsule VFS.
[[nodiscard]] bool read_capsule_file(capsule::Vfs& vfs,
                                     std::string_view path,
                                     std::vector<u8>& bytes) {
    auto opened = vfs.open(path, capsule::AccessMode::Read,
                           capsule::ShareMode::Read,
                           capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return false;

    bytes.clear();
    if (auto info = vfs.stat_handle(opened.value()); info.is_ok()) {
        bytes.reserve(static_cast<usize>(info.value().size));
    }
    std::array<u8, 64 * 1024> chunk{};
    bool ok = true;
    for (;;) {
        auto read = vfs.read(opened.value(), chunk);
        if (read.is_err()) { ok = false; break; }
        if (read.value() == 0) break;
        bytes.insert(bytes.end(), chunk.begin(),
                     chunk.begin() + static_cast<std::ptrdiff_t>(read.value()));
    }
    (void)vfs.close(opened.value());
    return ok;
}

/// Widen ANSI (the type-library tables are ANSI) for registry values, which
/// Vitreo stores as UTF-16 the way Windows does.
[[nodiscard]] std::wstring widen(std::string_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (const char c : text) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return result;
}

/// Everything up to the last separator — RegisterTypeLib's default HELPDIR.
[[nodiscard]] std::string directory_of(std::string_view path) {
    const auto slash = path.find_last_of("\\/");
    if (slash == std::string_view::npos) return {};
    return std::string(path.substr(0, slash));
}

/// Compare a guest IID against a known interface.
[[nodiscard]] bool iid_matches(Marshaler& m, GuestAddr iid_ptr,
                               const std::array<u8, 16>& expected) {
    std::array<u8, 16> actual{};
    if (m.memory().read_bytes(iid_ptr, reinterpret_cast<Byte*>(actual.data()),
                              actual.size()).is_err()) {
        return false;
    }
    return actual == expected;
}

} // namespace

u32 OleAut32Manager::load_type_lib(Marshaler& m) {
    const GuestAddr file_ptr = m.arg_ptr(0);
    const GuestAddr out_ptr  = m.arg_ptr(1);
    if (out_ptr == 0) return E_INVALIDARG;
    (void)m.write_ptr(out_ptr, 0);
    if (file_ptr == 0) return E_INVALIDARG;

    auto path = m.read_wide_string(file_ptr);
    if (path.is_err() || path.value().empty()) {
        VITREO_TRACE("[oleaut32] LoadTypeLib: unreadable szFile=0x%llx -> "
                     "E_INVALIDARG\n",
                     static_cast<unsigned long long>(file_ptr));
        return E_INVALIDARG;
    }
    return load_type_lib_from_path(m, path.value(), out_ptr);
}

u32 OleAut32Manager::load_type_lib_ex(Marshaler& m) {
    // LoadTypeLibEx(szFile, regkind, pptlib). REGKIND_REGISTER (0) is the
    // default and additionally registers the library; REGKIND_NONE (1) and
    // REGKIND_DEFAULT (2) only load it.
    constexpr u32 REGKIND_DEFAULT  = 0;
    constexpr u32 REGKIND_REGISTER = 1;

    const GuestAddr file_ptr = m.arg_ptr(0);
    const u32       regkind  = static_cast<u32>(m.arg(1));
    const GuestAddr out_ptr  = m.arg_ptr(2);
    if (out_ptr == 0) return E_INVALIDARG;
    (void)m.write_ptr(out_ptr, 0);
    if (file_ptr == 0) return E_INVALIDARG;

    auto path = m.read_wide_string(file_ptr);
    if (path.is_err() || path.value().empty()) return E_INVALIDARG;

    const u32 hr = load_type_lib_from_path(m, path.value(), out_ptr);
    if (hr != S_OK) return hr;
    if (regkind != REGKIND_DEFAULT && regkind != REGKIND_REGISTER) return S_OK;

    const auto object = m.read_ptr(out_ptr);
    if (object.is_err()) return S_OK;
    auto state = find_typelib(static_cast<GuestAddr>(object.value()));
    if (state == nullptr) return S_OK;
    if (!write_typelib_registration(state->attributes, state->path,
                                    directory_of(state->path))) {
        return TYPE_E_REGISTRYACCESS;
    }
    return S_OK;
}

u32 OleAut32Manager::load_type_lib_from_path(Marshaler& m,
                                             const std::string& path,
                                             GuestAddr out_ptr) {
    if (m_vfs == nullptr) {
        VITREO_TRACE("[oleaut32] LoadTypeLib(\"%s\"): no capsule VFS bound -> "
                     "TYPE_E_CANTLOADLIBRARY\n", path.c_str());
        return TYPE_E_CANTLOADLIBRARY;
    }

    auto parsed_result = parse_type_lib_from_path(path);
    if (!parsed_result) {
        VITREO_TRACE("[oleaut32] LoadTypeLib(\"%s\"): not readable or not "
                     "a supported type library -> TYPE_E_CANTLOADLIBRARY\n",
                     path.c_str());
        return TYPE_E_CANTLOADLIBRARY;
    }
    TypeLibParseResult parsed = std::move(*parsed_result);

    const GuestAddr object = create_typelib_object(m);
    if (object == 0) {
        VITREO_TRACE("[oleaut32] LoadTypeLib(\"%s\"): out of scratch/thunks -> "
                     "E_OUTOFMEMORY\n", path.c_str());
        return E_OUTOFMEMORY;
    }

    auto state = std::make_shared<LoadedTypeLib>();
    state->attributes = std::move(*parsed.attributes);
    state->typeinfos  = std::move(parsed.typeinfos);
    state->path       = path;
    state->object     = object;
    const std::string libid = format_guid_braced(state->attributes.libid);
    const u16 major = state->attributes.major;
    const u16 minor = state->attributes.minor;
    const u32 type_infos = state->attributes.type_info_count;
    const std::string name = state->attributes.name;

    if (m.write_ptr(out_ptr, object).is_err()) return E_INVALIDARG;
    {
        std::lock_guard lock(m_typelibs_mutex);
        m_typelibs[object] = std::move(state);
    }

    VITREO_TRACE("[oleaut32] LoadTypeLib(\"%s\") -> ITypeLib=0x%08x libid=%s "
                 "name=\"%s\" version=%u.%u typeinfos=%u\n",
                 path.c_str(), static_cast<unsigned>(object), libid.c_str(),
                 name.c_str(), major, minor, type_infos);
    return S_OK;
}

std::optional<TypeLibParseResult> OleAut32Manager::parse_type_lib_from_path(
    const std::string& path) {
    if (m_vfs == nullptr) return std::nullopt;

    std::vector<u8> image;
    std::string module_path = path;
    std::optional<u32> resource_id;
    bool read = read_capsule_file(*m_vfs, module_path, image);
    if (!read) {
        const usize slash = path.find_last_of("\\/");
        if (slash != std::string::npos && slash + 1 < path.size()) {
            u64 parsed_id = 0;
            bool decimal = true;
            for (usize i = slash + 1; i < path.size(); ++i) {
                if (path[i] < '0' || path[i] > '9') {
                    decimal = false;
                    break;
                }
                parsed_id = parsed_id * 10u +
                    static_cast<u64>(path[i] - '0');
                if (parsed_id > std::numeric_limits<u32>::max()) {
                    decimal = false;
                    break;
                }
            }
            if (decimal) {
                module_path = path.substr(0, slash);
                resource_id = static_cast<u32>(parsed_id);
                read = read_capsule_file(*m_vfs, module_path, image);
            }
        }
    }
    if (!read) {
        return std::nullopt;
    }

    TypeLibParseResult parsed;
    if (!resource_id) {
        parsed = parse_typelib(
            std::span<const u8>(image.data(), image.size()));
    }
    if (!parsed.ok()) {
        // Learn: LoadTypeLib on a DLL/EXE loads the first TYPELIB resource.
        auto embedded = extract_typelib_resource_from_pe(
            std::span<const u8>(image.data(), image.size()), resource_id);
        if (!embedded.empty()) {
            parsed = parse_typelib(
                std::span<const u8>(embedded.data(), embedded.size()));
            if (parsed.ok()) {
                VITREO_TRACE(
                    "[oleaut32] LoadTypeLib(\"%s\"): extracted TYPELIB "
                    "resource%s%u (%zu bytes)\n",
                    path.c_str(), resource_id ? " " : " #",
                    resource_id.value_or(1), embedded.size());
            }
        }
    }
    if (!parsed.ok()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<TypeInfoSummary>
OleAut32Manager::resolve_registered_type_info(const TypeLibGuid& iid) {
    if (m_registry == nullptr || m_vfs == nullptr) return std::nullopt;

    const std::wstring interface_key = L"Interface\\" +
        widen(format_guid_braced(iid)) + L"\\TypeLib";
    auto key = m_registry->open_key(
        capsule::HKEY_CLASSES_ROOT, interface_key, false);
    if (key.is_err()) return std::nullopt;
    auto libid_value = m_registry->query_value(key.value(), L"");
    auto version_value = m_registry->query_value(key.value(), L"Version");
    m_registry->close_key(key.value());
    if (libid_value.is_err() || version_value.is_err() ||
        libid_value.value().type != capsule::RegType::Sz ||
        version_value.value().type != capsule::RegType::Sz) {
        return std::nullopt;
    }
    const auto* libid = std::get_if<std::wstring>(&libid_value.value().data);
    const auto* version =
        std::get_if<std::wstring>(&version_value.value().data);
    if (libid == nullptr || version == nullptr || libid->empty() ||
        version->empty()) {
        return std::nullopt;
    }

    const std::wstring typelib_base =
        L"TypeLib\\" + *libid + L"\\" + *version + L"\\0\\";
    std::string path;
    for (const wchar_t* platform : {L"win32", L"win64"}) {
        auto path_key = m_registry->open_key(
            capsule::HKEY_CLASSES_ROOT, typelib_base + platform, false);
        if (path_key.is_err()) continue;
        auto path_value = m_registry->query_value(path_key.value(), L"");
        m_registry->close_key(path_key.value());
        if (path_value.is_err() ||
            path_value.value().type != capsule::RegType::Sz) {
            continue;
        }
        const auto* path_w =
            std::get_if<std::wstring>(&path_value.value().data);
        if (path_w != nullptr && !path_w->empty()) {
            path.reserve(path_w->size());
            for (const wchar_t ch : *path_w) {
                path.push_back(static_cast<char>(ch & 0xFF));
            }
            break;
        }
    }
    if (path.empty()) return std::nullopt;

    auto parsed = parse_type_lib_from_path(path);
    if (!parsed) return std::nullopt;
    const auto index = parsed->find_typeinfo_by_guid(iid);
    if (!index || *index >= parsed->typeinfos.size()) return std::nullopt;
    return parsed->typeinfos[*index];
}

GuestAddr OleAut32Manager::create_typelib_object(Marshaler& m) {
    if (m_thunks == nullptr) return 0;

    static constexpr std::array<std::string_view, TYPELIB_METHOD_COUNT>
        method_names{{
            "Vitreo_TypeLib_QueryInterface",
            "Vitreo_TypeLib_AddRef",
            "Vitreo_TypeLib_Release",
            "Vitreo_TypeLib_GetTypeInfoCount",
            "Vitreo_TypeLib_GetTypeInfo",
            "Vitreo_TypeLib_GetTypeInfoType",
            "Vitreo_TypeLib_GetTypeInfoOfGuid",
            "Vitreo_TypeLib_GetLibAttr",
            "Vitreo_TypeLib_GetTypeComp",
            "Vitreo_TypeLib_GetDocumentation",
            "Vitreo_TypeLib_IsName",
            "Vitreo_TypeLib_FindName",
            "Vitreo_TypeLib_ReleaseTLibAttr",
        }};

    std::array<GuestAddr, TYPELIB_METHOD_COUNT> methods{};
    for (usize i = 0; i < method_names.size(); ++i) {
        auto thunk = m_thunks->get_thunk_address("oleaut32.dll", method_names[i]);
        if (thunk.is_err()) return 0;
        methods[i] = thunk.value();
    }

    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr object =
        scratch_alloc(pointer_size * static_cast<u32>(1 + methods.size()));
    if (object == 0) return 0;

    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return 0;
    for (usize i = 0; i < methods.size(); ++i) {
        if (m.write_ptr(vtable + static_cast<GuestAddr>(i) * pointer_size,
                        methods[i]).is_err()) {
            return 0;
        }
    }
    return object;
}

GuestAddr OleAut32Manager::create_typeinfo_object(
    Marshaler& m, const std::shared_ptr<LoadedTypeLib>& parent, u32 index) {
    if (m_thunks == nullptr || parent == nullptr) return 0;
    if (index >= parent->typeinfos.size()) return 0;

    static constexpr std::array<std::string_view, TYPEINFO_METHOD_COUNT>
        method_names{{
            "Vitreo_TypeInfo_QueryInterface",
            "Vitreo_TypeInfo_AddRef",
            "Vitreo_TypeInfo_Release",
            "Vitreo_TypeInfo_GetTypeAttr",
            "Vitreo_TypeInfo_GetTypeComp",
            "Vitreo_TypeInfo_GetFuncDesc",
            "Vitreo_TypeInfo_GetVarDesc",
            "Vitreo_TypeInfo_GetNames",
            "Vitreo_TypeInfo_GetRefTypeOfImplType",
            "Vitreo_TypeInfo_GetImplTypeFlags",
            "Vitreo_TypeInfo_GetIDsOfNames",
            "Vitreo_TypeInfo_Invoke",
            "Vitreo_TypeInfo_GetDocumentation",
            "Vitreo_TypeInfo_GetDllEntry",
            "Vitreo_TypeInfo_GetRefTypeInfo",
            "Vitreo_TypeInfo_AddressOfMember",
            "Vitreo_TypeInfo_CreateInstance",
            "Vitreo_TypeInfo_GetMops",
            "Vitreo_TypeInfo_GetContainingTypeLib",
            "Vitreo_TypeInfo_ReleaseTypeAttr",
            "Vitreo_TypeInfo_ReleaseFuncDesc",
            "Vitreo_TypeInfo_ReleaseVarDesc",
        }};

    std::array<GuestAddr, TYPEINFO_METHOD_COUNT> methods{};
    for (usize i = 0; i < method_names.size(); ++i) {
        auto thunk = m_thunks->get_thunk_address("oleaut32.dll", method_names[i]);
        if (thunk.is_err()) return 0;
        methods[i] = thunk.value();
    }

    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr object =
        scratch_alloc(pointer_size * static_cast<u32>(1 + methods.size()));
    if (object == 0) return 0;

    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return 0;
    for (usize i = 0; i < methods.size(); ++i) {
        if (m.write_ptr(vtable + static_cast<GuestAddr>(i) * pointer_size,
                        methods[i]).is_err()) {
            return 0;
        }
    }

    auto state = std::make_shared<LoadedTypeInfo>();
    state->parent = parent;
    state->index  = index;
    {
        std::lock_guard lock(m_typelibs_mutex);
        m_typeinfos[object] = std::move(state);
    }
    return object;
}

GuestAddr OleAut32Manager::alloc_bstr_from_ansi(Marshaler& m,
                                                std::string_view text) {
    std::vector<Byte> utf16(text.size() * 2);
    for (usize i = 0; i < text.size(); ++i) {
        utf16[i * 2]     = static_cast<Byte>(static_cast<unsigned char>(text[i]));
        utf16[i * 2 + 1] = 0;
    }
    return allocate_bstr_bytes(m, utf16);
}

std::shared_ptr<OleAut32Manager::LoadedTypeLib>
OleAut32Manager::find_typelib(GuestAddr object) {
    std::lock_guard lock(m_typelibs_mutex);
    auto it = m_typelibs.find(object);
    return it == m_typelibs.end() ? nullptr : it->second;
}

std::shared_ptr<OleAut32Manager::LoadedTypeInfo>
OleAut32Manager::find_typeinfo(GuestAddr object) {
    std::lock_guard lock(m_typelibs_mutex);
    auto it = m_typeinfos.find(object);
    return it == m_typeinfos.end() ? nullptr : it->second;
}

bool OleAut32Manager::write_typelib_registration(
    const TypeLibAttributes& attributes,
    const std::string& path,
    const std::string& help_dir) {
    if (m_registry == nullptr) return false;

    // Windows writes the version as hex without leading zeros, and keys the
    // platform subkey off SYSKIND ("win32" for SYS_WIN32, "win64" for Win64).
    char version[32];
    std::snprintf(version, sizeof(version), "%x.%x", attributes.major,
                  attributes.minor);
    char lcid[16];
    std::snprintf(lcid, sizeof(lcid), "%x", attributes.lcid);
    char flags[16];
    std::snprintf(flags, sizeof(flags), "%x", attributes.lib_flags);
    const char* platform =
        attributes.syskind == SysKind::Win64 ? "win64" : "win32";

    const std::string libid = format_guid_braced(attributes.libid);
    const std::wstring base =
        L"TypeLib\\" + widen(libid) + L"\\" + widen(version);

    const auto set_default = [&](const std::wstring& subkey,
                                 const std::wstring& value) -> bool {
        auto key = m_registry->open_key(capsule::HKEY_CLASSES_ROOT, subkey,
                                        /*create=*/true);
        if (key.is_err()) return false;
        const bool ok =
            m_registry->set_value(key.value(), L"",
                                  capsule::RegValue{capsule::RegType::Sz, value})
                .is_ok();
        m_registry->close_key(key.value());
        return ok;
    };

    bool ok = set_default(base, widen(attributes.doc));
    ok = set_default(base + L"\\" + widen(lcid) + L"\\" + widen(platform),
                     widen(path)) && ok;
    ok = set_default(base + L"\\FLAGS", widen(flags)) && ok;
    ok = set_default(base + L"\\HELPDIR", widen(help_dir)) && ok;

    VITREO_TRACE("[oleaut32] RegisterTypeLib: HKCR\\TypeLib\\%s\\%s\\%s\\%s = "
                 "\"%s\" -> %s\n",
                 libid.c_str(), version, lcid, platform, path.c_str(),
                 ok ? "ok" : "FAILED");
    return ok;
}

u32 OleAut32Manager::register_type_lib(Marshaler& m) {
    const GuestAddr object    = m.arg_ptr(0);
    const GuestAddr path_ptr  = m.arg_ptr(1);
    const GuestAddr help_ptr  = m.arg_ptr(2);
    if (object == 0) return E_INVALIDARG;

    auto state = find_typelib(object);
    if (state == nullptr) {
        // The pointer did not come from Vitreo's LoadTypeLib, so there are no
        // attributes to register. Registering nothing and reporting success
        // would leave the caller believing its CLSIDs are resolvable.
        VITREO_TRACE("[oleaut32] RegisterTypeLib: unknown ITypeLib=0x%08x -> "
                     "E_INVALIDARG\n", static_cast<unsigned>(object));
        return E_INVALIDARG;
    }

    std::string path = state->path;
    if (path_ptr != 0) {
        if (auto given = m.read_wide_string(path_ptr);
            given.is_ok() && !given.value().empty()) {
            path = given.value();
        }
    }
    std::string help_dir = directory_of(path);
    if (help_ptr != 0) {
        if (auto given = m.read_wide_string(help_ptr); given.is_ok()) {
            help_dir = given.value();
        }
    }

    if (!write_typelib_registration(state->attributes, path, help_dir)) {
        return TYPE_E_REGISTRYACCESS;
    }
    return S_OK;
}

namespace {

[[nodiscard]] std::string narrow_path(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t ch : text) {
        result.push_back(static_cast<char>(ch & 0xFF));
    }
    return result;
}

[[nodiscard]] bool is_hex_lcid_key(const std::wstring& name) {
    if (name.empty()) return false;
    for (const wchar_t ch : name) {
        const bool digit = (ch >= L'0' && ch <= L'9');
        const bool hex_lo = (ch >= L'a' && ch <= L'f');
        const bool hex_hi = (ch >= L'A' && ch <= L'F');
        if (!digit && !hex_lo && !hex_hi) return false;
    }
    return true;
}

} // namespace

u32 OleAut32Manager::resolve_reg_typelib_path(const TypeLibGuid& libid,
                                              u16 major, u16 minor, u32 lcid,
                                              std::string& out_path) {
    out_path.clear();
    if (m_registry == nullptr) return TYPE_E_LIBNOTREGISTERED_HR;

    const std::string libid_text = format_guid_braced(libid);
    char version[32];
    std::snprintf(version, sizeof(version), "%x.%x", major, minor);
    const std::wstring version_key =
        L"TypeLib\\" + widen(libid_text) + L"\\" + widen(version);

    auto version_handle =
        m_registry->open_key(capsule::HKEY_CLASSES_ROOT, version_key,
                             /*create=*/false);
    if (version_handle.is_err()) {
        VITREO_TRACE("[oleaut32] LoadRegTypeLib: no HKCR\\TypeLib\\%s\\%s\n",
                     libid_text.c_str(), version);
        return TYPE_E_LIBNOTREGISTERED_HR;
    }

    const auto try_lcid = [&](u32 try_lcid) -> bool {
        char lcid_text[16];
        std::snprintf(lcid_text, sizeof(lcid_text), "%x", try_lcid);
        const std::wstring path_key =
            version_key + L"\\" + widen(lcid_text) + L"\\win32";
        auto key = m_registry->open_key(capsule::HKEY_CLASSES_ROOT, path_key,
                                        /*create=*/false);
        if (key.is_err()) return false;
        auto value = m_registry->query_value(key.value(), L"");
        m_registry->close_key(key.value());
        if (value.is_err() || value.value().type != capsule::RegType::Sz) {
            return false;
        }
        const auto* path_w = std::get_if<std::wstring>(&value.value().data);
        if (path_w == nullptr || path_w->empty()) return false;
        out_path = narrow_path(*path_w);
        return true;
    };

    // Current locale search order: requested, neutral, then a registered LCID.
    bool found = try_lcid(lcid);
    if (!found && lcid != 0) found = try_lcid(0);
    if (!found && lcid != 0x409) found = try_lcid(0x409);
    if (!found) {
        auto subkeys = m_registry->enum_keys(version_handle.value());
        if (subkeys.is_ok()) {
            for (const auto& name : subkeys.value()) {
                if (!is_hex_lcid_key(name)) continue; // skip FLAGS / HELPDIR
                char dummy[16];
                // Re-open via try_lcid after parsing the hex name.
                unsigned parsed = 0;
                for (wchar_t ch : name) {
                    parsed <<= 4;
                    if (ch >= L'0' && ch <= L'9') parsed |= static_cast<unsigned>(ch - L'0');
                    else if (ch >= L'a' && ch <= L'f') parsed |= static_cast<unsigned>(ch - L'a' + 10);
                    else if (ch >= L'A' && ch <= L'F') parsed |= static_cast<unsigned>(ch - L'A' + 10);
                }
                (void)dummy;
                if (try_lcid(parsed)) {
                    found = true;
                    break;
                }
            }
        }
    }

    m_registry->close_key(version_handle.value());
    if (!found) {
        VITREO_TRACE("[oleaut32] LoadRegTypeLib: %s %s lcid=0x%x has no win32 "
                     "path\n",
                     libid_text.c_str(), version, lcid);
        return TYPE_E_LIBNOTREGISTERED_HR;
    }
    return S_OK;
}

u32 OleAut32Manager::load_reg_type_lib(Marshaler& m) {
    // HRESULT LoadRegTypeLib(REFGUID rguid, WORD wVerMajor, WORD wVerMinor,
    //                        LCID lcid, ITypeLib** pptlib);
    const GuestAddr guid_ptr = m.arg_ptr(0);
    const u16 major = static_cast<u16>(m.arg(1));
    const u16 minor = static_cast<u16>(m.arg(2));
    const u32 lcid  = static_cast<u32>(m.arg(3));
    const GuestAddr out_ptr = m.arg_ptr(4);

    if (out_ptr == 0) return E_INVALIDARG;
    (void)m.write_ptr(out_ptr, 0);
    if (guid_ptr == 0) return E_INVALIDARG;

    TypeLibGuid libid{};
    if (m.memory().read_bytes(guid_ptr, reinterpret_cast<Byte*>(libid.data()),
                              libid.size()).is_err()) {
        return E_INVALIDARG;
    }

    std::string path;
    const u32 path_hr =
        resolve_reg_typelib_path(libid, major, minor, lcid, path);
    if (path_hr != S_OK) {
        VITREO_TRACE("[oleaut32] LoadRegTypeLib(%s, %u.%u, lcid=0x%x) -> "
                     "TYPE_E_LIBNOTREGISTERED\n",
                     format_guid_braced(libid).c_str(), major, minor, lcid);
        return path_hr;
    }

    VITREO_TRACE("[oleaut32] LoadRegTypeLib(%s, %u.%u, lcid=0x%x) -> \"%s\"\n",
                 format_guid_braced(libid).c_str(), major, minor, lcid,
                 path.c_str());
    return load_type_lib_from_path(m, path, out_ptr);
}

u32 OleAut32Manager::unregister_type_lib(Marshaler& m) {
    const GuestAddr libid_ptr = m.arg_ptr(0);
    const u16 major = static_cast<u16>(m.arg(1));
    const u16 minor = static_cast<u16>(m.arg(2));
    if (libid_ptr == 0 || m_registry == nullptr) return E_INVALIDARG;

    TypeLibGuid libid{};
    if (m.memory().read_bytes(libid_ptr, reinterpret_cast<Byte*>(libid.data()),
                              libid.size()).is_err()) {
        return E_INVALIDARG;
    }

    char version[32];
    std::snprintf(version, sizeof(version), "%x.%x", major, minor);
    const std::string libid_text = format_guid_braced(libid);
    const std::wstring parent = L"TypeLib\\" + widen(libid_text);

    auto key = m_registry->open_key(capsule::HKEY_CLASSES_ROOT, parent,
                                    /*create=*/false);
    if (key.is_err()) {
        VITREO_TRACE("[oleaut32] UnRegisterTypeLib %s %s -> "
                     "TYPE_E_LIBNOTREGISTERED\n", libid_text.c_str(), version);
        return TYPE_E_LIBNOTREGISTERED_HR;
    }
    const bool removed = m_registry->delete_key(key.value(), widen(version)).is_ok();
    m_registry->close_key(key.value());

    VITREO_TRACE("[oleaut32] UnRegisterTypeLib %s %s -> %s\n",
                 libid_text.c_str(), version, removed ? "S_OK" : "not-found");
    return removed ? S_OK : TYPE_E_LIBNOTREGISTERED_HR;
}

// ----- ITypeLib vtable -----

u32 OleAut32Manager::typelib_query_interface(Marshaler& m) {
    const GuestAddr object  = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (out_ptr == 0) return E_INVALIDARG;
    (void)m.write_ptr(out_ptr, 0);
    if (iid_ptr == 0) return E_INVALIDARG;

    auto state = find_typelib(object);
    if (state == nullptr) return E_INVALIDARG;

    if (!iid_matches(m, iid_ptr, IID_IUNKNOWN_BYTES) &&
        !iid_matches(m, iid_ptr, IID_ITYPELIB_BYTES)) {
        return E_NOINTERFACE;
    }
    if (m.write_ptr(out_ptr, object).is_err()) return E_INVALIDARG;
    {
        std::lock_guard lock(m_typelibs_mutex);
        ++state->ref_count;
    }
    return S_OK;
}

u32 OleAut32Manager::typelib_add_ref(Marshaler& m) {
    auto state = find_typelib(m.arg_ptr(0));
    if (state == nullptr) return 0;
    std::lock_guard lock(m_typelibs_mutex);
    return ++state->ref_count;
}

u32 OleAut32Manager::typelib_release(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    std::lock_guard lock(m_typelibs_mutex);
    auto it = m_typelibs.find(object);
    if (it == m_typelibs.end()) return 0;
    if (--it->second->ref_count != 0) return it->second->ref_count;

    // The scratch region is a bump allocator, so the object's guest bytes stay
    // mapped; dropping the state is what makes further calls fail loudly
    // rather than operating on a released library.
    m_typelibs.erase(it);
    return 0;
}

u32 OleAut32Manager::typelib_get_type_info_count(Marshaler& m) {
    auto state = find_typelib(m.arg_ptr(0));
    return state == nullptr ? 0 : state->attributes.type_info_count;
}

u32 OleAut32Manager::typelib_get_lib_attr(Marshaler& m) {
    const GuestAddr object  = m.arg_ptr(0);
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (out_ptr == 0) return E_INVALIDARG;
    if (m.write_ptr(out_ptr, 0).is_err()) return E_INVALIDARG;

    auto state = find_typelib(object);
    if (state == nullptr) return E_INVALIDARG;

    const GuestAddr attr = oa_alloc(m, TLIBATTR_SIZE);
    if (attr == 0) return E_OUTOFMEMORY;

    const auto& a = state->attributes;
    bool ok = m.memory().write_bytes(
                  attr, reinterpret_cast<const Byte*>(a.libid.data()),
                  a.libid.size()).is_ok();
    ok = m.write_u32(attr + 16, a.lcid).is_ok() && ok;
    ok = m.write_u32(attr + 20, static_cast<u32>(a.syskind)).is_ok() && ok;
    ok = m.write_u16(attr + 24, a.major).is_ok() && ok;
    ok = m.write_u16(attr + 26, a.minor).is_ok() && ok;
    ok = m.write_u16(attr + 28, a.lib_flags | 8u).is_ok() && ok;
    ok = m.write_u16(attr + 30, 0).is_ok() && ok;
    if (!ok) { oa_free(attr); return E_OUTOFMEMORY; }

    if (m.write_ptr(out_ptr, attr).is_err()) { oa_free(attr); return E_INVALIDARG; }
    return S_OK;
}

u32 OleAut32Manager::typelib_release_tlib_attr(Marshaler& m) {
    oa_free(m.arg_ptr(1));
    return 0;
}

u32 OleAut32Manager::typelib_get_documentation(Marshaler& m) {
    const GuestAddr object       = m.arg_ptr(0);
    const i32       index        = static_cast<i32>(m.arg(1));
    const GuestAddr name_ptr     = m.arg_ptr(2);
    const GuestAddr doc_ptr      = m.arg_ptr(3);
    const GuestAddr context_ptr  = m.arg_ptr(4);
    const GuestAddr helpfile_ptr = m.arg_ptr(5);

    auto state = find_typelib(object);
    if (state == nullptr) return E_INVALIDARG;

    const std::string* name = nullptr;
    const std::string* doc  = nullptr;
    if (index == -1) {
        name = &state->attributes.name;
        doc  = &state->attributes.doc;
    } else if (index >= 0 &&
               static_cast<u32>(index) < state->typeinfos.size()) {
        name = &state->typeinfos[static_cast<u32>(index)].name;
        doc  = &state->typeinfos[static_cast<u32>(index)].doc;
    } else {
        VITREO_TRACE("[oleaut32] ITypeLib::GetDocumentation index=%d -> "
                     "TYPE_E_ELEMENTNOTFOUND\n", index);
        return TYPE_E_ELEMENTNOTFOUND;
    }

    if (name_ptr != 0) {
        const GuestAddr bstr = alloc_bstr_from_ansi(m, *name);
        if (bstr == 0) return E_OUTOFMEMORY;
        if (m.write_ptr(name_ptr, bstr).is_err()) return E_INVALIDARG;
    }
    if (doc_ptr != 0) {
        const GuestAddr bstr = alloc_bstr_from_ansi(m, *doc);
        if (bstr == 0) return E_OUTOFMEMORY;
        if (m.write_ptr(doc_ptr, bstr).is_err()) return E_INVALIDARG;
    }
    if (context_ptr != 0) (void)m.write_u32(context_ptr, 0);
    if (helpfile_ptr != 0) (void)m.write_ptr(helpfile_ptr, 0);
    return S_OK;
}

u32 OleAut32Manager::typelib_get_type_info(Marshaler& m) {
    const GuestAddr object  = m.arg_ptr(0);
    const u32       index   = static_cast<u32>(m.arg(1));
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (out_ptr == 0) return E_INVALIDARG;
    (void)m.write_ptr(out_ptr, 0);

    auto state = find_typelib(object);
    if (state == nullptr) return E_INVALIDARG;
    if (index >= state->typeinfos.size()) {
        VITREO_TRACE("[oleaut32] ITypeLib::GetTypeInfo index=%u -> "
                     "TYPE_E_ELEMENTNOTFOUND\n", index);
        return TYPE_E_ELEMENTNOTFOUND;
    }

    const GuestAddr typeinfo = create_typeinfo_object(m, state, index);
    if (typeinfo == 0) return E_OUTOFMEMORY;
    if (m.write_ptr(out_ptr, typeinfo).is_err()) return E_INVALIDARG;
    {
        std::lock_guard lock(m_typelibs_mutex);
        ++state->ref_count;
    }
    VITREO_TRACE("[oleaut32] ITypeLib::GetTypeInfo index=%u -> "
                 "ITypeInfo=0x%08x name=\"%s\"\n",
                 index, static_cast<unsigned>(typeinfo),
                 state->typeinfos[index].name.c_str());
    return S_OK;
}

u32 OleAut32Manager::typelib_get_type_info_type(Marshaler& m) {
    const GuestAddr object  = m.arg_ptr(0);
    const u32       index   = static_cast<u32>(m.arg(1));
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (out_ptr == 0) return E_INVALIDARG;
    (void)m.write_u32(out_ptr, 0);

    auto state = find_typelib(object);
    if (state == nullptr) return E_INVALIDARG;
    if (index >= state->typeinfos.size()) return TYPE_E_ELEMENTNOTFOUND;

    const u32 tkind = state->typeinfos[index].typekind & 0xFu;
    if (m.write_u32(out_ptr, tkind).is_err()) return E_INVALIDARG;
    return S_OK;
}

u32 OleAut32Manager::typelib_get_type_info_of_guid(Marshaler& m) {
    const GuestAddr object   = m.arg_ptr(0);
    const GuestAddr guid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr  = m.arg_ptr(2);
    if (out_ptr == 0) return E_INVALIDARG;
    (void)m.write_ptr(out_ptr, 0);
    if (guid_ptr == 0) return E_INVALIDARG;

    auto state = find_typelib(object);
    if (state == nullptr) return E_INVALIDARG;

    TypeLibGuid guid{};
    if (m.memory().read_bytes(guid_ptr, reinterpret_cast<Byte*>(guid.data()),
                              guid.size()).is_err()) {
        return E_INVALIDARG;
    }

    std::optional<u32> index;
    for (u32 i = 0; i < state->typeinfos.size(); ++i) {
        if (state->typeinfos[i].has_guid && state->typeinfos[i].guid == guid) {
            index = i;
            break;
        }
    }
    if (!index.has_value()) {
        VITREO_TRACE("[oleaut32] ITypeLib::GetTypeInfoOfGuid(%s) -> "
                     "TYPE_E_ELEMENTNOTFOUND\n",
                     format_guid_braced(guid).c_str());
        return TYPE_E_ELEMENTNOTFOUND;
    }

    const GuestAddr typeinfo =
        create_typeinfo_object(m, state, index.value());
    if (typeinfo == 0) return E_OUTOFMEMORY;
    if (m.write_ptr(out_ptr, typeinfo).is_err()) return E_INVALIDARG;
    {
        std::lock_guard lock(m_typelibs_mutex);
        ++state->ref_count;
    }
    VITREO_TRACE("[oleaut32] ITypeLib::GetTypeInfoOfGuid(%s) -> "
                 "ITypeInfo=0x%08x index=%u name=\"%s\"\n",
                 format_guid_braced(guid).c_str(),
                 static_cast<unsigned>(typeinfo), index.value(),
                 state->typeinfos[index.value()].name.c_str());
    return S_OK;
}

u32 OleAut32Manager::typelib_get_type_comp(Marshaler& m) {
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (out_ptr != 0) (void)m.write_ptr(out_ptr, 0);
    VITREO_TRACE("[oleaut32] ITypeLib::GetTypeComp — no ITypeComp -> "
                 "E_NOTIMPL\n");
    return E_NOTIMPL;
}

u32 OleAut32Manager::typelib_is_name(Marshaler& m) {
    // Without the decoded name table Vitreo cannot say a name is present, and
    // claiming FALSE is the honest answer: pfName=FALSE means "not found".
    const GuestAddr found_ptr = m.arg_ptr(3);
    if (found_ptr != 0) (void)m.write_u32(found_ptr, 0);
    return S_OK;
}

u32 OleAut32Manager::typelib_find_name(Marshaler& m) {
    const GuestAddr found_ptr = m.arg_ptr(5);
    if (found_ptr != 0) (void)m.write_u16(found_ptr, 0);
    return S_OK;
}

// ----- ITypeInfo vtable -----

u32 OleAut32Manager::typeinfo_query_interface(Marshaler& m) {
    const GuestAddr object  = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (out_ptr == 0) return E_INVALIDARG;
    (void)m.write_ptr(out_ptr, 0);
    if (iid_ptr == 0) return E_INVALIDARG;

    auto state = find_typeinfo(object);
    if (state == nullptr) return E_INVALIDARG;

    if (!iid_matches(m, iid_ptr, IID_IUNKNOWN_BYTES) &&
        !iid_matches(m, iid_ptr, IID_ITYPEINFO_BYTES) &&
        !iid_matches(m, iid_ptr, IID_ITYPEINFO2_BYTES)) {
        TypeLibGuid iid{};
        if (m.memory()
                .read_bytes(iid_ptr, reinterpret_cast<Byte*>(iid.data()),
                            iid.size())
                .is_ok()) {
            VITREO_TRACE("[oleaut32] ITypeInfo::QueryInterface(%s) -> "
                         "E_NOINTERFACE\n",
                         format_guid_braced(iid).c_str());
        }
        return E_NOINTERFACE;
    }
    if (m.write_ptr(out_ptr, object).is_err()) return E_INVALIDARG;
    {
        std::lock_guard lock(m_typelibs_mutex);
        ++state->ref_count;
    }
    VITREO_TRACE("[oleaut32] ITypeInfo::QueryInterface index=%u -> S_OK "
                 "(ITypeInfo/ITypeInfo2/IUnknown)\n",
                 state->index);
    return S_OK;
}

u32 OleAut32Manager::typeinfo_add_ref(Marshaler& m) {
    auto state = find_typeinfo(m.arg_ptr(0));
    if (state == nullptr) return 0;
    std::lock_guard lock(m_typelibs_mutex);
    return ++state->ref_count;
}

u32 OleAut32Manager::typeinfo_release(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    std::shared_ptr<LoadedTypeLib> parent;
    u32 remaining = 0;
    {
        std::lock_guard lock(m_typelibs_mutex);
        auto it = m_typeinfos.find(object);
        if (it == m_typeinfos.end()) return 0;
        remaining = --it->second->ref_count;
        if (remaining == 0) {
            parent = it->second->parent;
            m_typeinfos.erase(it);
        }
    }
    if (remaining == 0 && parent != nullptr) {
        std::lock_guard lock(m_typelibs_mutex);
        if (parent->ref_count > 0) --parent->ref_count;
        if (parent->ref_count == 0 && parent->object != 0) {
            m_typelibs.erase(parent->object);
        }
    }
    return remaining;
}

u32 OleAut32Manager::typeinfo_get_type_attr(Marshaler& m) {
    const GuestAddr object  = m.arg_ptr(0);
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (out_ptr == 0) return E_INVALIDARG;
    if (m.write_ptr(out_ptr, 0).is_err()) return E_INVALIDARG;

    auto state = find_typeinfo(object);
    if (state == nullptr || state->parent == nullptr) return E_INVALIDARG;
    if (state->index >= state->parent->typeinfos.size()) return E_INVALIDARG;
    const TypeInfoSummary& info = state->parent->typeinfos[state->index];

    const bool is64 = m.cpu_is_64bit();
    const auto& abi = is64 ? typeinfo_layout::x64 : typeinfo_layout::x86;
    const u32 attr_size = abi.TYPEATTR_size;
    const GuestAddr attr = oa_alloc(m, attr_size);
    if (attr == 0) return E_OUTOFMEMORY;

    TypeLibGuid guid = info.has_guid ? info.guid : TypeLibGuid{};
    bool ok = m.memory()
                  .write_bytes(attr, reinterpret_cast<const Byte*>(guid.data()),
                               guid.size())
                  .is_ok();
    ok = m.write_u32(attr + 16, state->parent->attributes.lcid).is_ok() && ok;
    ok = m.write_u32(attr + 24, 0xffffffffu).is_ok() && ok;
    ok = m.write_u32(attr + 28, 0xffffffffu).is_ok() && ok;
    const u32 ptr_size = is64 ? 8u : 4u;
    const GuestAddr after_ptrs = attr + abi.TYPEATTR_cbSizeInstance;
    ok = m.write_u32(after_ptrs, info.reader_instance_size(ptr_size)).is_ok() && ok;
    ok = m.write_u32(after_ptrs + 4, info.typekind & 0xFu).is_ok() && ok;
    ok = m.write_u16(after_ptrs + 8, info.c_funcs).is_ok() && ok;
    ok = m.write_u16(after_ptrs + 10, info.c_vars).is_ok() && ok;
    ok = m.write_u16(after_ptrs + 12, info.c_impl_types).is_ok() && ok;
    ok = m.write_u16(after_ptrs + 14, static_cast<u16>(reader_vtable_bytes(
        info.cb_size_vft, state->parent->attributes.syskind, ptr_size))).is_ok() && ok;
    const u32 kind = info.typekind & 0x0fu;
    const bool cross_bitness = is64 != (state->parent->attributes.syskind == SysKind::Win64);
    const u16 alignment = cross_bitness && kind >= 3 && kind <= 5
        ? static_cast<u16>(ptr_size) : info.alignment;
    ok = m.write_u16(after_ptrs + 16, alignment).is_ok() && ok;
    ok = m.write_u16(after_ptrs + 18, info.type_flags).is_ok() && ok;
    ok = m.write_u16(after_ptrs + 20, info.major).is_ok() && ok;
    ok = m.write_u16(after_ptrs + 22, info.minor).is_ok() && ok;
    if (!ok) { oa_free(attr); return E_OUTOFMEMORY; }

    if (m.write_ptr(out_ptr, attr).is_err()) { oa_free(attr); return E_INVALIDARG; }
    VITREO_TRACE("[oleaut32] ITypeInfo::GetTypeAttr index=%u name=\"%s\" "
                 "tkind=%u cFuncs=%u cVars=%u cImplTypes=%u flags=0x%x "
                 "-> attr=0x%08x\n",
                 state->index, info.name.c_str(), info.typekind & 0xFu,
                 info.c_funcs, info.c_vars, info.c_impl_types,
                 info.type_flags, static_cast<unsigned>(attr));
    return S_OK;
}

u32 OleAut32Manager::typeinfo_release_type_attr(Marshaler& m) {
    oa_free(m.arg_ptr(1));
    return 0;
}

u32 OleAut32Manager::typeinfo_get_documentation(Marshaler& m) {
    const GuestAddr object       = m.arg_ptr(0);
    const i32       memid        = static_cast<i32>(m.arg(1));
    const GuestAddr name_ptr     = m.arg_ptr(2);
    const GuestAddr doc_ptr      = m.arg_ptr(3);
    const GuestAddr context_ptr  = m.arg_ptr(4);
    const GuestAddr helpfile_ptr = m.arg_ptr(5);

    auto state = find_typeinfo(object);
    if (state == nullptr || state->parent == nullptr) return E_INVALIDARG;
    if (state->index >= state->parent->typeinfos.size()) return E_INVALIDARG;

    const TypeInfoSummary& info = state->parent->typeinfos[state->index];
    std::string_view name;
    std::string_view doc;
    if (memid == -1) {
        name = info.name;
        doc  = info.doc;
    } else {
        const FuncSummary* fn = nullptr;
        for (const FuncSummary& f : info.funcs) {
            if (f.memid == memid) { fn = &f; break; }
        }
        if (fn == nullptr) {
            VITREO_TRACE("[oleaut32] ITypeInfo::GetDocumentation memid=%d — "
                         "unknown member -> TYPE_E_ELEMENTNOTFOUND\n", memid);
            return TYPE_E_ELEMENTNOTFOUND;
        }
        name = fn->name;
    }

    if (name_ptr != 0) {
        const GuestAddr bstr = alloc_bstr_from_ansi(m, name);
        if (bstr == 0) return E_OUTOFMEMORY;
        if (m.write_ptr(name_ptr, bstr).is_err()) return E_INVALIDARG;
    }
    if (doc_ptr != 0) {
        const GuestAddr bstr = alloc_bstr_from_ansi(m, doc);
        if (bstr == 0) return E_OUTOFMEMORY;
        if (m.write_ptr(doc_ptr, bstr).is_err()) return E_INVALIDARG;
    }
    if (context_ptr != 0) (void)m.write_u32(context_ptr, 0);
    if (helpfile_ptr != 0) (void)m.write_ptr(helpfile_ptr, 0);
    return S_OK;
}

u32 OleAut32Manager::typeinfo_get_containing_type_lib(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr pptlib = m.arg_ptr(1);
    const GuestAddr pindex = m.arg_ptr(2);

    auto state = find_typeinfo(object);
    if (state == nullptr || state->parent == nullptr) return E_INVALIDARG;

    if (pptlib != 0) {
        if (m.write_ptr(pptlib, state->parent->object).is_err()) {
            return E_INVALIDARG;
        }
        std::lock_guard lock(m_typelibs_mutex);
        ++state->parent->ref_count;
    }
    if (pindex != 0) {
        if (m.write_u32(pindex, state->index).is_err()) return E_INVALIDARG;
    }
    return S_OK;
}

u32 OleAut32Manager::typeinfo_get_type_comp(Marshaler& m) {
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (out_ptr != 0) (void)m.write_ptr(out_ptr, 0);
    return E_NOTIMPL;
}

u32 OleAut32Manager::typeinfo_get_func_desc(Marshaler& m) {
    const GuestAddr object  = m.arg_ptr(0);
    const u32       index   = static_cast<u32>(m.arg(1));
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (out_ptr == 0 || m.write_ptr(out_ptr, 0).is_err()) return E_INVALIDARG;

    auto state = find_typeinfo(object);
    if (state == nullptr || state->parent == nullptr) return E_INVALIDARG;
    if (state->index >= state->parent->typeinfos.size()) return E_INVALIDARG;
    const TypeInfoSummary& info = state->parent->typeinfos[state->index];
    if (index >= info.funcs.size()) {
        VITREO_TRACE("[oleaut32] ITypeInfo::GetFuncDesc index=%u func=%u "
                     "name=\"%s\" -> TYPE_E_ELEMENTNOTFOUND\n",
                     state->index, index, info.name.c_str());
        return TYPE_E_ELEMENTNOTFOUND;
    }
    const FuncSummary& fn = info.funcs[index];

    const bool is64 = m.cpu_is_64bit();
    const auto& abi = is64 ? typeinfo_layout::x64 : typeinfo_layout::x86;
    if (fn.param_descriptions.size() != fn.c_params || fn.result_description.tags.empty())
        return E_NOTIMPL;
    const auto unsupported_type = [](const TypeDescription& type) {
        return std::find(type.tags.begin(), type.tags.end(), 28u) != type.tags.end();
    };
    const bool has_default = std::any_of(fn.param_flags.begin(), fn.param_flags.end(),
        [](u16 flags) { return (flags & 0x20u) != 0; });
    if (has_default || unsupported_type(fn.result_description) ||
        std::any_of(fn.param_descriptions.begin(), fn.param_descriptions.end(), unsupported_type)) {
        static std::once_flag warning;
        std::call_once(warning, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED oleaut32!ITypeInfo::GetFuncDesc: "
                                 "default values or C-array descriptions\n");
        });
        return E_NOTIMPL;
    }
    usize child_count = fn.result_description.tags.size() - 1;
    for (const auto& param : fn.param_descriptions) {
        if (param.tags.empty()) return E_NOTIMPL;
        child_count += param.tags.size() - 1;
    }
    const usize size = abi.FUNCDESC_size + fn.c_params * abi.ELEMDESC_size +
                       child_count * abi.TYPEDESC_size;
    const GuestAddr desc = oa_alloc(m, size);
    if (desc == 0) return E_OUTOFMEMORY;
    const GuestAddr parameters = desc + abi.FUNCDESC_size;
    GuestAddr next_child = parameters + fn.c_params * abi.ELEMDESC_size;
    auto write_type = [&](GuestAddr destination, const TypeDescription& type) {
        bool ok = true;
        for (usize index = 0; index < type.tags.size(); ++index) {
            ok = m.write_u16(destination + abi.TYPEDESC_vt, type.tags[index]).is_ok() && ok;
            if (index + 1 < type.tags.size()) {
                ok = m.write_ptr(destination, next_child).is_ok() && ok;
                destination = next_child;
                next_child += abi.TYPEDESC_size;
            } else if (type.tags[index] == 29) {
                ok = m.write_u32(destination, type.reference).is_ok() && ok;
            }
        }
        return ok;
    };
    bool ok = m.write_u32(desc, static_cast<u32>(fn.memid)).is_ok();
    ok = m.write_ptr(desc + abi.FUNCDESC_lprgelemdescParam,
                     fn.c_params ? parameters : 0).is_ok() && ok;
    ok = m.write_u32(desc + abi.FUNCDESC_funckind, fn.funckind).is_ok() && ok;
    ok = m.write_u32(desc + abi.FUNCDESC_invkind, fn.invkind).is_ok() && ok;
    ok = m.write_u32(desc + abi.FUNCDESC_callconv, fn.callconv).is_ok() && ok;
    ok = m.write_u16(desc + abi.FUNCDESC_cParams, fn.c_params).is_ok() && ok;
    ok = m.write_u16(desc + abi.FUNCDESC_cParamsOpt, static_cast<u16>(fn.optional_params)).is_ok() && ok;
    ok = m.write_u16(desc + abi.FUNCDESC_oVft, static_cast<u16>(reader_vtable_bytes(
        fn.o_vft, state->parent->attributes.syskind, is64 ? 8u : 4u))).is_ok() && ok;
    ok = m.write_u16(desc + abi.FUNCDESC_wFuncFlags, fn.function_flags).is_ok() && ok;
    ok = write_type(desc + abi.FUNCDESC_elemdescFunc, fn.result_description) && ok;
    for (usize index = 0; index < fn.c_params; ++index) {
        const GuestAddr param = parameters + index * abi.ELEMDESC_size;
        ok = write_type(param, fn.param_descriptions[index]) && ok;
        ok = m.write_u16(param + abi.ELEMDESC_paramdesc + abi.PARAMDESC_wParamFlags,
                         fn.param_flags[index]).is_ok() && ok;
    }
    if (!ok) { oa_free(desc); return E_OUTOFMEMORY; }
    if (m.write_ptr(out_ptr, desc).is_err()) { oa_free(desc); return E_INVALIDARG; }
    return S_OK;
}

u32 OleAut32Manager::typeinfo_get_var_desc(Marshaler& m) {
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (out_ptr != 0) (void)m.write_ptr(out_ptr, 0);
    return TYPE_E_ELEMENTNOTFOUND;
}

u32 OleAut32Manager::typeinfo_get_names(Marshaler& m) {
    // ITypeInfo::GetNames(this, memid, rgBstrNames, cMaxNames, pcNames)
    const GuestAddr object      = m.arg_ptr(0);
    const i32       memid       = static_cast<i32>(m.arg(1));
    const GuestAddr names_ptr   = m.arg_ptr(2);
    const u32       c_max_names = static_cast<u32>(m.arg(3));
    const GuestAddr pc_names    = m.arg_ptr(4);

    auto state = find_typeinfo(object);
    if (state == nullptr || state->parent == nullptr) return E_INVALIDARG;
    if (state->index >= state->parent->typeinfos.size()) return E_INVALIDARG;
    const TypeInfoSummary& info = state->parent->typeinfos[state->index];

    const FuncSummary* fn = nullptr;
    for (const FuncSummary& f : info.funcs) {
        if (f.memid == memid) { fn = &f; break; }
    }
    if (fn == nullptr) {
        VITREO_TRACE("[oleaut32] ITypeInfo::GetNames memid=%d name=\"%s\" "
                     "-> TYPE_E_ELEMENTNOTFOUND\n",
                     memid, info.name.c_str());
        if (pc_names != 0) (void)m.write_u32(pc_names, 0);
        return TYPE_E_ELEMENTNOTFOUND;
    }

    u32 written = 0;
    if (c_max_names > 0 && names_ptr != 0) {
        const GuestAddr bstr = alloc_bstr_from_ansi(m, fn->name);
        if (bstr == 0) return E_OUTOFMEMORY;
        if (m.write_ptr(names_ptr, bstr).is_err()) return E_INVALIDARG;
        written = 1;
    }
    if (pc_names != 0) (void)m.write_u32(pc_names, written);
    VITREO_TRACE("[oleaut32] ITypeInfo::GetNames memid=%d -> \"%s\" "
                 "(cMax=%u wrote=%u)\n",
                 memid, fn->name.c_str(), c_max_names, written);
    return S_OK;
}

u32 OleAut32Manager::typeinfo_get_ref_type_of_impl_type(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32       index  = static_cast<u32>(m.arg(1));
    const GuestAddr out_ptr = m.arg_ptr(2);

    auto state = find_typeinfo(object);
    if (state == nullptr || state->parent == nullptr) return E_INVALIDARG;
    if (state->index >= state->parent->typeinfos.size()) return E_INVALIDARG;
    const TypeInfoSummary& info = state->parent->typeinfos[state->index];
    if (index >= info.impl_hrefs.size()) {
        if (out_ptr != 0) (void)m.write_u32(out_ptr, 0);
        return TYPE_E_ELEMENTNOTFOUND;
    }
    if (out_ptr != 0) {
        (void)m.write_u32(out_ptr, info.impl_hrefs[index]);
    }
    VITREO_TRACE("[oleaut32] ITypeInfo::GetRefTypeOfImplType index=%u "
                 "impl=%u -> href=0x%x\n",
                 state->index, index, info.impl_hrefs[index]);
    return S_OK;
}

u32 OleAut32Manager::typeinfo_get_impl_type_flags(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32       index  = static_cast<u32>(m.arg(1));
    const GuestAddr out_ptr = m.arg_ptr(2);

    auto state = find_typeinfo(object);
    if (state == nullptr || state->parent == nullptr) return E_INVALIDARG;
    if (state->index >= state->parent->typeinfos.size()) return E_INVALIDARG;
    const TypeInfoSummary& info = state->parent->typeinfos[state->index];
    if (index >= info.impl_hrefs.size()) {
        if (out_ptr != 0) (void)m.write_u32(out_ptr, 0);
        return TYPE_E_ELEMENTNOTFOUND;
    }
    // IMPLTYPEFLAG_FDEFAULT is common for the dual/base slot; 0 is honest.
    if (out_ptr != 0) (void)m.write_u32(out_ptr, 0);
    return S_OK;
}

u32 OleAut32Manager::typeinfo_get_ids_of_names(Marshaler& m) {
    // ITypeInfo::GetIDsOfNames(this, rgszNames, cNames, pMemId)
    // Dual IDispatchImpl delegates here. A stub DISP_E_UNKNOWNNAME surfaces
    // as script error 0x80020006.
    const GuestAddr object     = m.arg_ptr(0);
    const GuestAddr rgsz_names = m.arg_ptr(1);
    const u32       c_names    = static_cast<u32>(m.arg(2));
    const GuestAddr memid_ptr  = m.arg_ptr(3);

    auto state = find_typeinfo(object);
    if (state == nullptr || state->parent == nullptr) return E_INVALIDARG;
    if (state->index >= state->parent->typeinfos.size()) return E_INVALIDARG;
    if (rgsz_names == 0 || memid_ptr == 0 || c_names == 0) return E_INVALIDARG;

    auto read_ole_name = [&](GuestAddr name_ptr) -> std::string {
        if (name_ptr == 0) return {};
        std::string out;
        for (u32 i = 0; i < 256; ++i) {
            auto ch = m.read_u16(name_ptr + i * 2);
            if (!ch.is_ok()) break;
            if (ch.value() == 0) break;
            const u16 c = ch.value();
            out.push_back(c < 128 ? static_cast<char>(c) : '?');
        }
        return out;
    };

    auto iequals = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (usize i = 0; i < a.size(); ++i) {
            const auto ca = static_cast<unsigned char>(a[i]);
            const auto cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;
        }
        return true;
    };

    auto name0_ptr_r = m.read_ptr(rgsz_names);
    if (!name0_ptr_r.is_ok()) return E_INVALIDARG;
    const std::string name0 =
        read_ole_name(static_cast<GuestAddr>(name0_ptr_r.value()));

    // Search this type's functions before its base interface.
    constexpr u32 TYPEINFO_BASE_SIZE = 0x64;
    const auto& typeinfos = state->parent->typeinfos;
    std::vector<u32> visit_stack;
    visit_stack.push_back(state->index);
    std::vector<bool> visited(typeinfos.size(), false);

    const FuncSummary* fn = nullptr;
    const TypeInfoSummary* found_in = nullptr;
    while (!visit_stack.empty()) {
        const u32 idx = visit_stack.back();
        visit_stack.pop_back();
        if (idx >= typeinfos.size() || visited[idx]) continue;
        visited[idx] = true;
        const TypeInfoSummary& info = typeinfos[idx];
        for (const FuncSummary& f : info.funcs) {
            if (iequals(f.name, name0)) {
                fn = &f;
                found_in = &info;
                break;
            }
        }
        if (fn) break;
        if (!info.impl_hrefs.empty()) {
            const u32 href = info.impl_hrefs[0];
            if ((href & 3u) == 0) {
                const u32 base_idx = href / TYPEINFO_BASE_SIZE;
                if (base_idx < typeinfos.size()) {
                    visit_stack.push_back(base_idx);
                }
            }
        }
    }

    if (fn == nullptr || found_in == nullptr) {
        VITREO_TRACE("[oleaut32] ITypeInfo::GetIDsOfNames type=\"%s\" "
                     "name=\"%s\" -> DISP_E_UNKNOWNNAME\n",
                     typeinfos[state->index].name.c_str(), name0.c_str());
        for (u32 i = 0; i < c_names; ++i) {
            (void)m.write_u32(memid_ptr + i * 4, 0xFFFFFFFFu); // DISPID_UNKNOWN
        }
        return 0x80020006u; // DISP_E_UNKNOWNNAME
    }

    (void)m.write_u32(memid_ptr, static_cast<u32>(fn->memid));
    for (u32 i = 1; i < c_names; ++i) {
        (void)m.write_u32(memid_ptr + i * 4, 0xFFFFFFFFu);
    }
    VITREO_TRACE("[oleaut32] ITypeInfo::GetIDsOfNames type=\"%s\" "
                 "name=\"%s\" -> memid=%d via=\"%s\" (cNames=%u)\n",
                 typeinfos[state->index].name.c_str(), name0.c_str(),
                 fn->memid, found_in->name.c_str(), c_names);
    if (c_names > 1) return 0x80020006u;
    return S_OK;
}

u32 OleAut32Manager::typeinfo_invoke(Marshaler& m) {
    const GuestAddr object      = m.arg_ptr(0);
    const GuestAddr inst        = m.arg_ptr(1);
    const i32       memid       = static_cast<i32>(m.arg(2));
    const u32       flags       = static_cast<u32>(m.arg(3));
    const GuestAddr params_ptr  = m.arg_ptr(4);
    const GuestAddr result_ptr  = m.arg_ptr(5);
    const GuestAddr excep_ptr   = m.arg_ptr(6);
    const GuestAddr argerr_ptr  = m.arg_ptr(7);
    GuestAddr caller_ret = 0;
    GuestAddr parent_ret = 0;
    GuestAddr grandparent_ret = 0;
    if (m.has_cpu()) {
        auto* cpu = m.cpu();
        caller_ret = cpu->is_64bit()
            ? static_cast<GuestAddr>(
                  m.memory().read_u64(static_cast<GuestAddr>(cpu->gpr.rsp))
                      .value_or(0))
            : static_cast<GuestAddr>(
                  m.memory().read_u32(static_cast<GuestAddr>(cpu->sp()))
                      .value_or(0));
        if (!cpu->is_64bit() && cpu->gpr.rbp != 0) {
            const GuestAddr frame =
                static_cast<GuestAddr>(cpu->gpr.rbp);
            parent_ret = static_cast<GuestAddr>(
                m.memory().read_u32(frame + 4).value_or(0));
            const GuestAddr parent_frame = static_cast<GuestAddr>(
                m.memory().read_u32(frame).value_or(0));
            if (parent_frame != 0) {
                grandparent_ret = static_cast<GuestAddr>(
                    m.memory().read_u32(parent_frame + 4).value_or(0));
            }
        }
    }
    (void)params_ptr;
    (void)excep_ptr;

    auto state = find_typeinfo(object);
    if (state == nullptr || state->parent == nullptr) return E_INVALIDARG;
    if (state->index >= state->parent->typeinfos.size()) return E_INVALIDARG;
    const auto& typeinfos = state->parent->typeinfos;

    // Follow the base interface when the member is not on this typeinfo; dual
    // interfaces keep base members (e.g. ISetupTextSubstitution.Value) only
    // on the parent typeinfo.
    constexpr u32 TYPEINFO_BASE_SIZE = 0x64;
    const FuncSummary* fn = nullptr;
    {
        std::vector<u32> visit_stack{state->index};
        std::vector<bool> visited(typeinfos.size(), false);
        while (!visit_stack.empty() && fn == nullptr) {
            const u32 idx = visit_stack.back();
            visit_stack.pop_back();
            if (idx >= typeinfos.size() || visited[idx]) continue;
            visited[idx] = true;
            const TypeInfoSummary& info = typeinfos[idx];
            for (const FuncSummary& f : info.funcs) {
                if (f.memid != memid) continue;
                if ((flags & f.invkind) != 0 || f.invkind == 0) {
                    fn = &f;
                    break;
                }
                if (fn == nullptr) fn = &f;
            }
            if (fn) break;
            if (!info.impl_hrefs.empty()) {
                const u32 href = info.impl_hrefs[0];
                if ((href & 3u) == 0) {
                    const u32 base_idx = href / TYPEINFO_BASE_SIZE;
                    if (base_idx < typeinfos.size()) visit_stack.push_back(base_idx);
                }
            }
        }
    }
    const TypeInfoSummary& info = typeinfos[state->index];
    if (fn == nullptr || inst == 0 || fn->o_vft == 0 || !m_guest_call) {
        VITREO_TRACE("[oleaut32] ITypeInfo::Invoke this=0x%08x inst=0x%08x "
                     "memid=%d flags=0x%x name=\"%s\" -> E_NOTIMPL "
                     "(no vtable target)\n",
                     static_cast<unsigned>(object), static_cast<unsigned>(inst),
                     memid, flags, info.name.c_str());
        return E_NOTIMPL;
    }

    // Supported patterns (MSDN ITypeInfo::Invoke / indexed properties):
    //   cParams=1  [retval]*                 — _NewEnum / simple getters
    //   cParams=1  [in] T                    — simple PROPERTYPUT
    //   cParams=2  [in] T, [out,retval]*     — indexed PROPERTYGET
    //   cParams=2  [in] T, [in] U            — indexed PROPERTYPUT
    //                                          (TextSub.Value = …)
    //   cParams=3  [in] T, [in] U, [out,retval]* — ValueEx-style GET
    const bool has_retval = fn->retval_vt != 0;
    const u16 in_argc =
        has_retval ? static_cast<u16>(fn->c_params - 1) : fn->c_params;
    const bool supported =
        (fn->c_params == 1 && has_retval && in_argc == 0) ||
        (fn->c_params == 1 && !has_retval && in_argc == 1) ||
        (fn->c_params == 2 && has_retval && in_argc == 1) ||
        (fn->c_params == 2 && !has_retval && in_argc == 2) ||
        (fn->c_params == 3 && has_retval && in_argc == 2);
    if (!supported) {
        VITREO_TRACE("[oleaut32] ITypeInfo::Invoke memid=%d name=\"%s\" "
                     "cParams=%u retval_vt=%u invkind=%u — signature not "
                     "implemented -> E_NOTIMPL\n",
                     memid, fn->name.c_str(), fn->c_params, fn->retval_vt,
                     fn->invkind);
        return E_NOTIMPL;
    }
    auto vtable_r = m.read_ptr(inst);
    if (!vtable_r.is_ok() || vtable_r.value() == 0) return E_INVALIDARG;
    const GuestAddr vtable = static_cast<GuestAddr>(vtable_r.value());
    // FUNCDESC::oVft in the loaded SYS_WIN32 type library is expressed in
    // four-byte vtable slots. A 64-bit instance uses eight-byte entries, so
    // preserve the method index while adapting the byte offset.
    const GuestAddr method_offset = m.cpu_is_64bit()
        ? static_cast<GuestAddr>(fn->o_vft) * 2u
        : static_cast<GuestAddr>(fn->o_vft);
    auto method_r = m.read_ptr(vtable + method_offset);
    if (!method_r.is_ok() || method_r.value() == 0) return E_INVALIDARG;
    const GuestAddr method = static_cast<GuestAddr>(method_r.value());

    // Extract [in] args from DISPPARAMS. rgvarg is reverse order:
    //   GET indexed:  rgvarg[0]=index
    //   PUT indexed:  rgvarg[0]=newVal, rgvarg[1]=index  (MSDN)
    //   PUT simple:   rgvarg[0]=newVal
    // Vtable IDL order is (this, index…, [newVal|retval*]).
    auto read_variant_payload = [&](GuestAddr var, u64& out) -> u32 {
        const u16 vt = static_cast<u16>(m.read_u16(var).value_or(0));
        const u16 base_vt = static_cast<u16>(vt & 0xFFF);
        const bool is_array = (vt & 0x2000) != 0; // VT_ARRAY
        if ((vt & 0x4000) != 0) {
            const GuestAddr ptr =
                static_cast<GuestAddr>(m.read_ptr(var + 8).value_or(0));
            if (ptr == 0) return DISP_E_TYPEMISMATCH;
            if (is_array) {
                // VT_BYREF|VT_ARRAY|* — VARIANT.pparray → SAFEARRAY*
                out = m.read_ptr(ptr).value_or(0);
            } else if (base_vt == VT_VARIANT) {
                // VT_BYREF|VT_VARIANT — pass the pointed-to VARIANT*
                out = ptr;
            } else if (base_vt == VT_BSTR || base_vt == VT_UNKNOWN ||
                       base_vt == VT_DISPATCH) {
                out = m.read_ptr(ptr).value_or(0);
            } else if (base_vt == VT_I4 || base_vt == VT_UI4) {
                out = m.read_u32(ptr).value_or(0);
            } else if (base_vt == VT_I2 || base_vt == VT_BOOL ||
                       base_vt == VT_UI2) {
                out = m.read_u16(ptr).value_or(0);
            } else if (base_vt == VT_UI1 || base_vt == VT_I1) {
                out = m.read_u8(ptr).value_or(0);
            } else {
                VITREO_TRACE("[oleaut32] ITypeInfo::Invoke memid=%d name=\"%s\" "
                             "BYREF in-arg vt=%u -> DISP_E_TYPEMISMATCH\n",
                             memid, fn->name.c_str(), vt);
                return DISP_E_TYPEMISMATCH;
            }
        } else if (is_array) {
            // VT_ARRAY|* — VARIANT.parray is LPSAFEARRAY (offset 8).
            // SAFEARRAY(T) vtable args take the array pointer, not &VARIANT.
            // Sequence arguments arrive as VT_ARRAY|VT_VARIANT.
            out = m.read_ptr(var + 8).value_or(0);
        } else if (base_vt == VT_VARIANT) {
            // Bare VT_VARIANT [in] — pass address of the VARIANT itself.
            out = var;
        } else if (base_vt == VT_BSTR || base_vt == VT_UNKNOWN ||
                   base_vt == VT_DISPATCH) {
            out = m.read_ptr(var + 8).value_or(0);
        } else if (base_vt == VT_I4 || base_vt == VT_UI4) {
            out = m.read_u32(var + 8).value_or(0);
        } else if (base_vt == VT_I2 || base_vt == VT_BOOL ||
                   base_vt == VT_UI2) {
            out = m.read_u16(var + 8).value_or(0);
        } else if (base_vt == VT_UI1 || base_vt == VT_I1) {
            out = m.read_u8(var + 8).value_or(0);
        } else {
            VITREO_TRACE("[oleaut32] ITypeInfo::Invoke memid=%d name=\"%s\" "
                         "in-arg vt=%u unsupported -> DISP_E_TYPEMISMATCH\n",
                         memid, fn->name.c_str(), vt);
            return DISP_E_TYPEMISMATCH;
        }
        return S_OK;
    };

    u64 in_args[2] = {0, 0};
    if (in_argc > 0) {
        if (params_ptr == 0) return E_INVALIDARG;
        const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
        const u32 variant_size = m.cpu_is_64bit() ? 24u : 16u;
        const GuestAddr rgvarg =
            static_cast<GuestAddr>(m.read_ptr(params_ptr).value_or(0));
        const u32 c_args =
            static_cast<u32>(
                m.read_u32(params_ptr + 2u * pointer_size).value_or(0));
        if (c_args < in_argc || rgvarg == 0) {
            VITREO_TRACE("[oleaut32] ITypeInfo::Invoke memid=%d name=\"%s\" "
                         "missing DISPPARAMS in-args (need=%u cArgs=%u) -> "
                         "DISP_E_BADPARAMCOUNT\n",
                         memid, fn->name.c_str(), in_argc, c_args);
            return 0x8002000Eu; // DISP_E_BADPARAMCOUNT
        }
        // Reverse rgvarg into IDL order: last rgvarg → first IDL [in].
        // ITypeInfo::Invoke coerces each argument to the FUNCDESC
        // parameter type (e.g. VT_I4 → VT_BSTR for TextSub.Value put).
        for (u16 i = 0; i < in_argc; ++i) {
            GuestAddr var =
                rgvarg +
                static_cast<u64>(in_argc - 1u - i) * variant_size;
            const u16 expect_vt =
                (i < fn->param_vts.size()) ? fn->param_vts[i] : 0;
            // Only coerce to concrete automation scalars/BSTR. Object and
            // VARIANT slots are left alone (USERDEFINED enums arrive as I4).
            const bool coerceable =
                expect_vt == VT_BSTR || expect_vt == VT_I2 ||
                expect_vt == VT_I4 || expect_vt == VT_UI4 ||
                expect_vt == VT_UI1 || expect_vt == VT_I1 ||
                expect_vt == VT_UI2 || expect_vt == VT_BOOL ||
                expect_vt == VT_R4 || expect_vt == VT_R8;
            {
                const u16 raw_vt =
                    static_cast<u16>(m.read_u16(var).value_or(0));
                const u16 have_vt = static_cast<u16>(raw_vt & 0xFFF);
                if (coerceable && have_vt != expect_vt) {
                    // Coerce BYREF forms too — VariantChangeType derefs them.
                    const GuestAddr coerced =
                        scratch_alloc(oleaut_variant_size(m.cpu_is_64bit()));
                    if (coerced == 0) return E_OUTOFMEMORY;
                    const u32 ch = variant_change_type_ex(
                        m, coerced, var, 0, expect_vt);
                    if (ch != S_OK) {
                        VITREO_TRACE("[oleaut32] ITypeInfo::Invoke memid=%d "
                                     "name=\"%s\" coerce arg%u vt=0x%x→%u "
                                     "hr=0x%08x\n",
                                     memid, fn->name.c_str(), i, raw_vt,
                                     expect_vt, ch);
                        if (argerr_ptr != 0) {
                            (void)m.write_u32(
                                argerr_ptr,
                                static_cast<u32>(in_argc - 1u - i));
                        }
                        return ch;
                    }
                    var = coerced;
                }
            }
            const u32 rh = read_variant_payload(var, in_args[i]);
            if (rh != S_OK) return rh;
        }
    }

    // [out,retval] VARIANT* — callee fills a VARIANT; we copy it to pvarResult.
    // Other retvals receive a pointer-sized slot (BSTR / I4 / punk).
    // PROPERTYPUT has no retval — skip the out slot.
    const bool retval_is_variant = has_retval && (fn->retval_vt == VT_VARIANT);
    GuestAddr out_slot = 0;
    if (has_retval) {
        const u32 out_size = retval_is_variant
            ? (m.cpu_is_64bit() ? 24u : 16u)
            : (m.cpu_is_64bit() ? 8u : 4u);
        out_slot = scratch_alloc(out_size);
        if (out_slot == 0) return E_OUTOFMEMORY;
        std::vector<Byte> zeros(out_size, 0);
        (void)m.memory().write_bytes(out_slot, zeros.data(), zeros.size());
    }

    u32 hr = 0;
    if (!has_retval) {
        // PROPERTYPUT: this, [in]…  (no retval pointer)
        if (in_argc == 1) {
            const u64 args[2] = {static_cast<u64>(inst), in_args[0]};
            hr = static_cast<u32>(
                m_guest_call(method, args, 2, m.cpu_is_64bit()));
        } else {
            const u64 args[3] = {static_cast<u64>(inst), in_args[0],
                                 in_args[1]};
            hr = static_cast<u32>(
                m_guest_call(method, args, 3, m.cpu_is_64bit()));
        }
    } else if (in_argc == 0) {
        const u64 args[2] = {static_cast<u64>(inst),
                             static_cast<u64>(out_slot)};
        hr = static_cast<u32>(
            m_guest_call(method, args, 2, m.cpu_is_64bit()));
    } else if (in_argc == 1) {
        // PROPERTYGET indexed: this, in_arg, [retval]*
        const u64 args[3] = {static_cast<u64>(inst), in_args[0],
                             static_cast<u64>(out_slot)};
        hr = static_cast<u32>(
            m_guest_call(method, args, 3, m.cpu_is_64bit()));
    } else {
        // PROPERTYGET two-index (e.g. TextSub.ValueEx): this, a, b, [retval]*
        const u64 args[4] = {static_cast<u64>(inst), in_args[0], in_args[1],
                             static_cast<u64>(out_slot)};
        hr = static_cast<u32>(
            m_guest_call(method, args, 4, m.cpu_is_64bit()));
    }

    GuestAddr out_val = 0;
    if (has_retval && !retval_is_variant) {
        out_val = static_cast<GuestAddr>(m.read_ptr(out_slot).value_or(0));
    }

    VITREO_TRACE("[oleaut32] ITypeInfo::Invoke this=0x%08x inst=0x%08x "
                 "memid=%d (%s) oVft=%u method=0x%08x -> hr=0x%08x out=0x%08x "
                 "retval_vt=%u in0=0x%llx in1=0x%llx caller_ret=0x%08x "
                 "parent_ret=0x%08x grandparent_ret=0x%08x\n",
                 static_cast<unsigned>(object), static_cast<unsigned>(inst),
                 memid, fn->name.c_str(), fn->o_vft,
                 static_cast<unsigned>(method), hr,
                 static_cast<unsigned>(out_val), fn->retval_vt,
                 static_cast<unsigned long long>(in_args[0]),
                 static_cast<unsigned long long>(in_args[1]),
                 static_cast<unsigned>(caller_ret),
                 static_cast<unsigned>(parent_ret),
                 static_cast<unsigned>(grandparent_ret));

    auto bstr_preview = [&](u64 addr) -> std::string {
        if (addr == 0 || addr < 0x10000) return {};
        const u32 byte_len =
            m.read_u32(static_cast<GuestAddr>(addr) - 4).value_or(0);
        if (byte_len == 0 || byte_len > 512) return {};
        std::string s;
        s.reserve(byte_len / 2);
        for (u32 i = 0; i + 1 < byte_len; i += 2) {
            const u16 ch =
                m.read_u16(static_cast<GuestAddr>(addr) + i).value_or(0);
            if (ch == 0) break;
            if (ch < 0x80) s.push_back(static_cast<char>(ch));
            else if (ch < 0x800) {
                s.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                s.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            } else {
                s.push_back(static_cast<char>(0xE0 | (ch >> 12)));
                s.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            }        }
        return s;
    };

    // TextSub.Value / ValueEx diagnostics: log the substitution key (and PUT
    // value when BSTR) so E_FAIL GETs identify the missing property.
    if ((fn->name == "Value" || fn->name == "ValueEx") && in_argc >= 1 &&
        in_args[0] != 0) {
        const std::string key = bstr_preview(in_args[0]);
        if (!has_retval) {
            const std::string val =
                bstr_preview(in_argc >= 2 ? in_args[in_argc - 1] : 0);
            VITREO_TRACE("[oleaut32] TextSub.%s PUT key=\"%s\" val=\"%s\" "
                         "hr=0x%08x inst=0x%08x\n",
                         fn->name.c_str(), key.c_str(),
                         val.empty() ? "(non-BSTR)" : val.c_str(), hr,
                         static_cast<unsigned>(inst));
        } else {
            const std::string out_s =
                (hr == S_OK && out_val != 0) ? bstr_preview(out_val) : "";
            VITREO_TRACE("[oleaut32] TextSub.%s GET key=\"%s\" -> hr=0x%08x "
                         "out=\"%s\" inst=0x%08x\n",
                         fn->name.c_str(), key.c_str(), hr,
                         (hr == S_OK) ? out_s.c_str() : "",
                         static_cast<unsigned>(inst));
            // Script engines read the substitution bag before it is filled.
            // A missing key returns E_FAIL and the script aborts with -5009
            // (0x8000ffff).
            //
            // A cold miss is an empty substitution: PUT "" then retry GET so
            // later path probes can proceed. Later PUTs overwrite the seeded
            // value. Do not invent a name for the missing key.
            // Value (non-Ex): only keys with the IFX_ prefix.
            // ValueEx: any key.
            const bool seed_empty =
                hr == E_FAIL &&
                (fn->name == "ValueEx" || key.rfind("IFX_", 0) == 0);
            if (seed_empty) {
                VITREO_TRACE("[oleaut32] TextSub cold miss: seeding empty key "
                             "\"%s\" via %s then retrying GET\n",
                             key.c_str(), fn->name.c_str());
                const GuestAddr empty =
                    sys_alloc_string_len(m, 0, 0); // SysAllocStringLen(NULL,0)
                // Prefer Value PUT for seeding: ValueEx PUT also returns E_FAIL
                // on a cold bag. Value PUT already accepts new keys
                // (UNINSTALL_STRING / UNINST / …). Dual: PUT = GET oVft+4.
                GuestAddr put_method = 0;
                bool put_is_valueex = false;
                if (fn->name == "ValueEx") {
                    for (const auto& f : info.funcs) {
                        if (f.name == "Value" && f.invkind == 2 /*PROPERTYGET*/ &&
                            f.o_vft != 0) {
                            const GuestAddr property_get_offset =
                                m.cpu_is_64bit()
                                ? static_cast<GuestAddr>(f.o_vft) * 2u
                                : static_cast<GuestAddr>(f.o_vft);
                            put_method = static_cast<GuestAddr>(
                                m.read_ptr(
                                    vtable + property_get_offset +
                                    (m.cpu_is_64bit() ? 8u : 4u)).value_or(0));
                            break;
                        }
                    }
                }
                if (put_method == 0) {
                    put_method = static_cast<GuestAddr>(
                        m.read_ptr(vtable + fn->o_vft + 4).value_or(0));
                    put_is_valueex = (fn->name == "ValueEx" && in_argc == 2);
                }
                if (empty != 0 && put_method != 0) {
                    u32 put_hr = E_FAIL;
                    if (put_is_valueex) {
                        const u64 put_args[4] = {
                            static_cast<u64>(inst), in_args[0], in_args[1],
                            static_cast<u64>(empty)};
                        put_hr = static_cast<u32>(
                            m_guest_call(put_method, put_args, 4, false));
                    } else {
                        const u64 put_args[3] = {static_cast<u64>(inst),
                                                in_args[0],
                                                static_cast<u64>(empty)};
                        put_hr = static_cast<u32>(
                            m_guest_call(put_method, put_args, 3, false));
                    }
                    VITREO_TRACE("[oleaut32] TextSub cold empty PUT "
                                 "key=\"%s\" hr=0x%08x\n",
                                 key.c_str(), put_hr);
                    if (put_hr == S_OK) {
                        (void)m.write_u32(out_slot, 0);
                        if (fn->name == "ValueEx" && in_argc == 2) {
                            const u64 retry_args[4] = {
                                static_cast<u64>(inst), in_args[0],
                                in_args[1], static_cast<u64>(out_slot)};
                            hr = static_cast<u32>(
                                m_guest_call(method, retry_args, 4, false));
                        } else {
                            const u64 retry_args[3] = {
                                static_cast<u64>(inst), in_args[0],
                                static_cast<u64>(out_slot)};
                            hr = static_cast<u32>(
                                m_guest_call(method, retry_args, 3, false));
                        }
                        out_val = static_cast<GuestAddr>(
                            m.read_ptr(out_slot).value_or(0));
                        VITREO_TRACE("[oleaut32] TextSub cold retry GET "
                                     "key=\"%s\" -> hr=0x%08x out=\"%s\"\n",
                                     key.c_str(), hr,
                                     bstr_preview(out_val).c_str());
                    } else if (empty != 0 && has_retval &&
                               fn->retval_vt == VT_BSTR) {
                        // Inherited Value PUT may be absent from this
                        // typeinfo's func list (ValueEx on ISetupTextSubstitution3).
                        // Return empty BSTR so the script sees a blank
                        // substitution rather than E_FAIL → -5009.
                        (void)m.write_ptr(out_slot, empty);
                        out_val = empty;
                        hr = S_OK;
                        VITREO_TRACE("[oleaut32] TextSub cold synthesize empty "
                                     "BSTR for key=\"%s\"\n",
                                     key.c_str());
                    }
                } else if (empty != 0 && has_retval && fn->retval_vt == VT_BSTR) {
                    (void)m.write_ptr(out_slot, empty);
                    out_val = empty;
                    hr = S_OK;
                    VITREO_TRACE("[oleaut32] TextSub cold synthesize empty "
                                 "BSTR for key=\"%s\" (no PUT slot)\n",
                                 key.c_str());
                }
            }
        }
    }

    // ISetupInfo::Info returns VARIANT — dump vt/payload so we can see whether
    // product-name (and friends) ever leave the kernel.
    if (fn->name == "Info" && retval_is_variant && out_slot != 0) {
        const u16 vt = static_cast<u16>(m.read_u16(out_slot).value_or(0));
        const u32 payload =
            static_cast<u32>(m.read_u32(out_slot + 8).value_or(0));
        std::string preview;
        if ((vt & 0xFFF) == VT_BSTR) preview = bstr_preview(payload);
        VITREO_TRACE("[oleaut32] ISetupInfo.Info(%u) -> hr=0x%08x vt=%u "
                     "payload=0x%x \"%s\" result_ptr=0x%08x\n",
                     static_cast<unsigned>(in_args[0]), hr, vt, payload,
                     preview.c_str(), static_cast<unsigned>(result_ptr));
    }

    if (hr != S_OK) {
        if (argerr_ptr != 0) (void)m.write_u32(argerr_ptr, 0);
        return hr;
    }

    // PROPERTYPUT ignores pVarResult (MSDN).
    if (has_retval && result_ptr != 0) {
        if (retval_is_variant) {
            // Copy the callee-filled VARIANT into the caller's result slot.
            const u32 vsz = oleaut_variant_size(m.cpu_is_64bit());
            std::vector<Byte> buf(vsz, 0);
            (void)m.memory().read_bytes(out_slot, buf.data(), vsz);
            if (m.memory().write_bytes(result_ptr, buf.data(), vsz).is_err()) {
                return E_OUTOFMEMORY;
            }
        } else {
            // Package the pointed-to [retval] value as a VARIANT. The MSFT
            // typedesc for the retval is VT_PTR→T; after the vtable call
            // `out_val` holds T (long / BSTR / IUnknown*).
            const u16 vt = fn->retval_vt != 0 ? fn->retval_vt : VT_UNKNOWN;
            const u32 vsz = oleaut_variant_size(m.cpu_is_64bit());
            std::vector<Byte> zeros(vsz, 0);
            if (m.memory().write_bytes(result_ptr, zeros.data(), vsz).is_err()) {
                return E_OUTOFMEMORY;
            }
            if (m.write_u16(result_ptr, vt).is_err()) return E_OUTOFMEMORY;
            if (m.write_ptr(result_ptr + 8, out_val).is_err()) {
                return E_OUTOFMEMORY;
            }
        }
    }
    return S_OK;
}

u32 OleAut32Manager::typeinfo_get_dll_entry(Marshaler& m) {
    (void)m;
    return TYPE_E_ELEMENTNOTFOUND;
}

u32 OleAut32Manager::typeinfo_get_ref_type_info(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32 reference = static_cast<u32>(m.arg(1));
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (out_ptr == 0 || m.write_ptr(out_ptr, 0).is_err()) return E_INVALIDARG;
    auto state = find_typeinfo(object);
    if (!state || !state->parent) return E_INVALIDARG;
    const auto& types = state->parent->typeinfos;
    for (u32 index = 0; index < types.size(); ++index) {
        if (types[index].reference != reference) continue;
        const GuestAddr result = create_typeinfo_object(m, state->parent, index);
        if (!result) return E_OUTOFMEMORY;
        if (m.write_ptr(out_ptr, result).is_err()) return E_INVALIDARG;
        std::lock_guard lock(m_typelibs_mutex);
        ++state->parent->ref_count;
        return S_OK;
    }
    return TYPE_E_ELEMENTNOTFOUND;
}

u32 OleAut32Manager::typeinfo_address_of_member(Marshaler& m) {
    (void)m;
    return TYPE_E_ELEMENTNOTFOUND;
}

u32 OleAut32Manager::typeinfo_create_instance(Marshaler& m) {
    const GuestAddr out_ptr = m.arg_ptr(3);
    if (out_ptr != 0) (void)m.write_ptr(out_ptr, 0);
    VITREO_TRACE("[oleaut32] ITypeInfo::CreateInstance — not implemented -> "
                 "E_NOTIMPL\n");
    return E_NOTIMPL;
}

u32 OleAut32Manager::typeinfo_get_mops(Marshaler& m) {
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (out_ptr != 0) (void)m.write_ptr(out_ptr, 0);
    return S_OK;
}

u32 OleAut32Manager::typeinfo_release_func_desc(Marshaler& m) {
    oa_free(m.arg_ptr(1));
    return 0;
}

u32 OleAut32Manager::typeinfo_release_var_desc(Marshaler& m) {
    (void)m;
    return 0;
}

} // namespace vitreo::winapi
