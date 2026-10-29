// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file typelib.hpp
/// @brief Reader for the on-disk Windows type-library (MSFT) container.
///
/// `LoadTypeLib` has to answer three questions about a .tlb before anything
/// else can happen: which LIBID it declares, which version/locale it is, and
/// what it calls itself. Those live in the fixed MSFT header and its segment
/// directory.
///
/// `ITypeLib::GetTypeInfoOfGuid` additionally needs the GUID table's
/// `hreftype` links into the TypeInfoBase segment. Vitreo parses that much so
/// a GUID lookup can hand back an `ITypeInfo`; methods that would need decoded
/// function/variable descriptions still fail honestly.

#include <vitreo/common.hpp>
#include "winapi/common/guid_text.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitreo::winapi {

/// A GUID exactly as it is stored on disk and passed across the Win32 ABI:
/// 16 raw bytes, Data1/Data2/Data3 little-endian.
using TypeLibGuid = std::array<u8, 16>;

/// SYSKIND values (the subset a 32-bit Windows type library can declare).
enum class SysKind : u32 {
    Win16 = 0,
    Win32 = 1,
    Mac   = 2,
    Win64 = 3,
};

/// The library-level attributes Vitreo reads out of a type library. These are
/// exactly the fields `ITypeLib::GetLibAttr` and `RegisterTypeLib` need.
struct TypeLibAttributes {
    TypeLibGuid libid{};
    u32         lcid{0};
    SysKind     syskind{SysKind::Win32};
    u16         major{0};
    u16         minor{0};
    u16         lib_flags{0};
    u32         type_info_count{0};
    std::string name;      ///< Library name from the name table ("" if absent).
    std::string doc;        ///< Help string from the string table ("" if absent).
};

/// Lossless pointer/SAFEARRAY chain for a public TYPEDESC. The last tag is
/// the terminal VARTYPE; reference is meaningful only for VT_USERDEFINED.
struct TypeDescription {
    std::vector<u16> tags;
    u32 reference{0};
};

/// Per-function summary from the MSFT member block (memoffset). Enough for
/// GetFuncDesc / GetDocumentation(memid) / Invoke-via-vtable.
struct FuncSummary {
    i32         memid{0};
    u16         o_vft{0};     ///< Byte offset into the instance vtable.
    u16         invkind{0};   ///< INVOKEKIND (PROPERTYGET=2, METHOD=1, …).
    u16         funckind{0};  ///< FUNCKIND (PUREVIRTUAL=1, DISPATCH=4, …).
    u16         c_params{0};
    u16         callconv{0};
    i16         optional_params{0};
    u16         function_flags{0};
    TypeDescription result_description;
    /// VARTYPE for the `[retval]` parameter after peeling one `VT_PTR`, ready
    /// to store into an Invoke result VARIANT (e.g. VT_I4 / VT_BSTR /
    /// VT_UNKNOWN). 0 when the member has no retval.
    u16         retval_vt{0};
    /// Per-parameter VARTYPEs in IDL order (size == c_params). For `[out]` /
    /// `[retval]` pointer params this is the pointed-to type after one
    /// `VT_PTR` peel — same convention as `retval_vt`. Used by
    /// `ITypeInfo::Invoke` to coerce DISPPARAMS args before the vtable call.
    std::vector<u16> param_vts;
    std::vector<TypeDescription> param_descriptions;
    /// PARAMFLAG_* bits for each parameter in IDL order. The universal
    /// marshaler needs directionality before deciding which guest memory must
    /// be copied into or out of the server process.
    std::vector<u16> param_flags;
    /// HREFTYPE for VT_USERDEFINED parameters, or -1 for predefined types.
    /// This preserves the public TYPEDESC relationship needed to distinguish
    /// interface pointers from enums and records.
    std::vector<i32> param_hrefs;
    /// Referenced type GUID for user-defined parameters when the type library
    /// supplies one. All-zero means the reference could not be resolved.
    std::vector<TypeLibGuid> param_iids;
    std::string name;
};

/// Native ITypeInfo exposes vtable slots at the reader's pointer width.
/// The file retains its SYSKIND; converting metadata must not change it.
[[nodiscard]] inline u32 reader_vtable_bytes(u32 file_bytes, SysKind file_kind,
                                            u32 reader_pointer_size) {
    const u32 file_pointer_size = file_kind == SysKind::Win64 ? 8u : 4u;
    return (file_bytes / file_pointer_size) * reader_pointer_size;
}

/// Per-typeinfo summary from MSFT TypeInfoBase (0x64 bytes each). Enough for
/// GetTypeInfoType / GetTypeAttr / GetDocumentation without decoding funcs.
struct TypeInfoSummary {
    u32 reference{0};          ///< Local reference from the file offset table.
    u16 alignment{0};
    TypeLibGuid guid{};           ///< Empty (all zero) when the type has no GUID.
    bool        has_guid{false};
    u32         typekind{0};      ///< Low nibble is TYPEKIND.
    u16         type_flags{0};
    u16         major{0};
    u16         minor{0};
    u32         cb_size_instance{0};
    u16         c_funcs{0};       ///< LOWORD of MSFT cElement.
    u16         c_vars{0};        ///< HIWORD of MSFT cElement.
    u16         c_impl_types{0};
    u16         cb_size_vft{0};
    std::string name;
    std::string doc;
    std::vector<FuncSummary> funcs; ///< Size c_funcs when the member block parsed.
    /// Implemented-interface HREFTYPEs (MSFT `datatype1` for INTERFACE/DISPATCH;
    /// coclass uses its linked reference records). Index 0 is the base
    /// interface used by GetIDsOfNames / GetNames.
    std::vector<u32> impl_hrefs;

    [[nodiscard]] u32 reader_instance_size(u32 pointer_size) const {
        // Interfaces and coclasses describe a pointer-sized instance. Native
        // x86/x64 observations are in typelib_fixture_probe.*.expected.
        const u32 kind = typekind & 0x0f;
        return kind >= 3 && kind <= 5 ? pointer_size : cb_size_instance;
    }

};

/// Why a byte range could not be read as a type library. Each value maps to a
/// distinct Windows-visible outcome, so callers never have to guess.
enum class TypeLibParseError {
    NotTypeLib,      ///< No MSFT signature — not a type library at all.
    UnsupportedFormat, ///< Recognised container, unsupported variant (e.g. SLTG).
    Truncated,       ///< MSFT signature present but the structure runs off the end.
};

/// Outcome of `parse_typelib`: attributes on success, a reason on failure.
struct TypeLibParseResult {
    std::optional<TypeLibAttributes> attributes;
    std::vector<TypeInfoSummary>     typeinfos; ///< Indexed 0..type_info_count-1.
    TypeLibParseError                error{TypeLibParseError::NotTypeLib};

    [[nodiscard]] bool ok() const noexcept { return attributes.has_value(); }

    /// Find a typeinfo index by GUID. Returns nullopt when the GUID is absent
    /// or only names the library / an imported type.
    [[nodiscard]] std::optional<u32> find_typeinfo_by_guid(
        const TypeLibGuid& guid) const;
};

/// Parse the library-level attributes (and per-typeinfo summaries) of a
/// type-library image.
///
/// Only the MSFT container (produced by MIDL and by every 32-bit Windows
/// toolchain since NT 3.5) is decoded. The older SLTG container is recognised
/// and reported as `UnsupportedFormat` so the caller can fail with the right
/// HRESULT rather than pretending the library loaded.
[[nodiscard]] TypeLibParseResult parse_typelib(std::span<const u8> image);

/// Learn LoadTypeLib: when `szFile` is a DLL/EXE, the type library is taken
/// from the first resource of type TYPELIB. Returns that blob, or empty when
/// the image is not a PE or has no TYPELIB resource.
[[nodiscard]] std::vector<u8> extract_typelib_resource_from_pe(
    std::span<const u8> pe_image,
    std::optional<u32> resource_id = std::nullopt);

/// Name of the parse failure, for logs.
[[nodiscard]] const char* typelib_parse_error_name(TypeLibParseError error) noexcept;

} // namespace vitreo::winapi
