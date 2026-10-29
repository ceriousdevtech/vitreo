// SPDX-License-Identifier: LGPL-2.1-only

/// @file oleaut32_registration.cpp
/// @brief Export table for oleaut32's BSTR, VARIANT and SAFEARRAY surface.
///
/// Composition only: maps export names to handlers and records each one's
/// stdcall byte count.
///
/// The handlers it names live in a nested `oleaut32` namespace — see the internal
/// header — because their spellings are shared vocabulary across the DLLs in
/// this family.
///
/// Split out of oleaut32.cpp, which was 2,488 lines.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.
///
/// VITREO_LARGE_FILE_OK: declarative ABI table — one entry per export. Plan
/// section 4.1 exempts these because length here tracks how many exports
/// Vitreo implements, not how much logic one file carries.

#include "core/cpu/state/cpu_state.hpp"
#include <cmath>
#include "oleaut32.hpp"
#include "winapi/com/oleaut32/oleaut32_internal2.hpp"
#include "winapi/com/oleaut32/oleaut32_internal.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace vitreo::winapi {

using namespace oleaut32;

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_oleaut32_thunks(OleAut32Manager& mgr) {
    DllExports exports;
    exports.entries.reserve(14);

    exports.entries.push_back(ExportEntry{
        "SysAllocString", 2,
        [&mgr](Marshaler& m) { impl_SysAllocString(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "SysAllocStringLen", 4,
        [&mgr](Marshaler& m) { impl_SysAllocStringLen(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "SysFreeString", 6,
        [&mgr](Marshaler& m) { impl_SysFreeString(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "SysStringLen", 7,
        [&mgr](Marshaler& m) { impl_SysStringLen(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "SysStringByteLen", 149,
        [&mgr](Marshaler& m) { impl_SysStringByteLen(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "VariantInit", 8,
        [&mgr](Marshaler& m) { impl_VariantInit(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "VariantClear", 9,
        [&mgr](Marshaler& m) { impl_VariantClear(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "VariantCopy", 10,
        [&mgr](Marshaler& m) { impl_VariantCopy(m, mgr); },
        8
    });
    // VariantCopyInd (ordinal 11). Non-BYREF variants follow VariantCopy.
    // The export is present at ordinal 11 with an 8-byte stdcall pop.
    // BYREF indirection is not decoded here.
    exports.entries.push_back(ExportEntry{
        "VariantCopyInd", 11,
        [&mgr](Marshaler& m) { impl_VariantCopy(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "VariantChangeType", 12,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0x80070057u /* E_INVALIDARG */); return; }
            impl_VariantChangeType(m, mgr);
        },
        16
    });
    exports.entries.push_back(ExportEntry{
        "SafeArrayCreate", 15,
        [&mgr](Marshaler& m) { impl_SafeArrayCreate(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "SafeArrayDestroy", 16,
        [&mgr](Marshaler& m) { impl_SafeArrayDestroy(m, mgr); },
        4
    });
    // The BSTR block occupies ordinals 2-7: SysAllocString(2),
    // SysReAllocString(3), SysAllocStringLen(4), SysReAllocStringLen(5),
    // SysFreeString(6), SysStringLen(7). The other five are registered above
    // at those ordinals. SysReAllocStringLen must be ordinal 5 as well as a
    // name: an import-by-ordinal that hits a fail-thunk stops the caller.
    exports.entries.push_back(ExportEntry{
        "SysReAllocStringLen", 5,
        [&mgr](Marshaler& m) { impl_SysReAllocStringLen(m, mgr); },
        12
    });
    // BOOL SysReAllocString(BSTR *pbstr, const OLECHAR *psz)
    //
    // Reallocate to hold the whole of `psz`. Expressed in terms of the length
    // form so both entry points share one allocator and one contract.
    exports.entries.push_back(ExportEntry{
        "SysReAllocString", 3,
        [&mgr](Marshaler& m) { impl_SysReAllocString(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "LoadTypeLib", 161,
        [&mgr](Marshaler& m) { impl_LoadTypeLib(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "RegisterTypeLib", 163,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0x80070057u /* E_INVALIDARG */); return; }
            impl_RegisterTypeLib(m, mgr);
        },
        12
    });
    exports.entries.push_back(ExportEntry{
        "OleCreateFontIndirect", 420,
        [&mgr](Marshaler& m) { impl_OleCreateFontIndirect(m, mgr); },
        12
    });

    // GetErrorInfo (ordinal 200) — 2 args: ULONG dwReserved, IErrorInfo **pperrinfo
    exports.entries.push_back(ExportEntry{
        "GetErrorInfo", 200,
        [](Marshaler& m) {
            // HRESULT GetErrorInfo(ULONG dwReserved, IErrorInfo **pperrinfo)
            constexpr u32 S_FALSE = 0x00000001u;
            constexpr u32 E_INVALIDARG = 0x80070057u;
            const u32 reserved = static_cast<u32>(m.arg(0));
            GuestAddr out = m.arg_ptr(1);
            if (reserved != 0 || out == 0) {
                m.set_return(E_INVALIDARG);
                return;
            }

            // IErrorInfo* has the guest pointer width. Writing a host-sized
            // eight-byte NULL into a PE32 caller's four-byte stack slot
            // corrupts the adjacent word (often the saved frame pointer).
            const auto write_result = m.cpu_is_64bit()
                ? m.write_u64(out, 0)
                : m.write_u32(out, 0);
            if (write_result.is_err()) {
                m.set_return(E_INVALIDARG);
                return;
            }
            m.set_return(S_FALSE); // No thread error-info object is available.
        },
        8
    });

    // SetErrorInfo (ordinal 201) — 2 args: ULONG dwReserved, IErrorInfo *perrinfo
    exports.entries.push_back(ExportEntry{
        "SetErrorInfo", 201,
        [](Marshaler& m) {
            // HRESULT SetErrorInfo(ULONG dwReserved, IErrorInfo *perrinfo)
            (void)m.arg(0); (void)m.arg(1);
            m.set_return(0u); // S_OK
        },
        8
    });

    // CreateErrorInfo (ordinal 202) — HRESULT CreateErrorInfo(ICreateErrorInfo**)
    // Reached on failure paths. Refuse with the 4-byte stdcall pop. A
    // missing-export stub returns 0xFFFFFFFF and pops nothing, which
    // unbalances the caller.
    exports.entries.push_back(ExportEntry{
        "CreateErrorInfo", 202,
        [](Marshaler& m) {
            constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
            const GuestAddr out = m.arg_ptr(0);
            if (out == 0) {
                m.set_return(0x80070057u); // E_INVALIDARG
                return;
            }
            if (m.cpu_is_64bit()) (void)m.write_u64(out, 0);
            else (void)m.write_u32(out, 0);
            m.set_return(E_OUTOFMEMORY);
        },
        4
    });

    // #150 is SysAllocStringByteLen (dumpbin /exports oleaut32.dll on
    // Windows 10); its length is in BYTES, not characters.
    exports.entries.push_back(ExportEntry{
        "SysAllocStringByteLen", 150,
        [&mgr](Marshaler& m) { impl_SysAllocStringByteLen(m, mgr); },
        8
    });

    // ------------------------------------------------------------------------
    // Export ordinals independently measured from both native system DLLs:
    // tools/oracle_probe/automation_export_ordinals_probe.{x86,x64}.expected.
    // ------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{ "SafeArrayGetDim", 17,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_get_dim(m)); }, 4 });
    exports.entries.push_back(ExportEntry{ "SafeArrayGetElemsize", 18,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_get_elemsize(m)); }, 4 });
    exports.entries.push_back(ExportEntry{ "SafeArrayGetUBound", 19,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_get_ubound(m)); }, 12 });
    exports.entries.push_back(ExportEntry{ "SafeArrayGetLBound", 20,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_get_lbound(m)); }, 12 });
    exports.entries.push_back(ExportEntry{ "SafeArrayLock", 21,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_lock(m)); }, 4 });
    exports.entries.push_back(ExportEntry{ "SafeArrayUnlock", 22,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_unlock(m)); }, 4 });
    exports.entries.push_back(ExportEntry{ "SafeArrayAccessData", 23,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_access_data(m)); }, 8 });
    exports.entries.push_back(ExportEntry{ "SafeArrayUnaccessData", 24,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_unaccess_data(m)); }, 4 });
    exports.entries.push_back(ExportEntry{ "SafeArrayGetElement", 25,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_get_element(m)); }, 12 });
    exports.entries.push_back(ExportEntry{ "SafeArrayPutElement", 26,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_put_element(m)); }, 12 });
    exports.entries.push_back(ExportEntry{ "SafeArrayCopy", 27,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_copy(m)); }, 8 });
    // #148 SafeArrayPtrOfIndex(psa, rgIndices, ppvData). dumpbin: ordinal 148.
    exports.entries.push_back(ExportEntry{ "SafeArrayPtrOfIndex", 148,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_ptr_of_index(m)); }, 12 });
    exports.entries.push_back(ExportEntry{ "SafeArrayGetVartype", 77,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_get_vartype(m)); }, 8 });

    // #28 DispGetParam — imported by ordinal.
    exports.entries.push_back(ExportEntry{ "DispGetParam", 28,
        [&mgr](Marshaler& m){ m.set_return(mgr.disp_get_param(m)); }, 20 });

    // #40 SafeArrayRedim — imported by ordinal.
    exports.entries.push_back(ExportEntry{ "SafeArrayRedim", 40,
        [&mgr](Marshaler& m){ m.set_return(mgr.safe_array_redim(m)); }, 8 });

    // #185 VariantTimeToSystemTime(double, SYSTEMTIME*) — 12 stdcall bytes.
    // Previously a guessed 16-byte stub at this ordinal.
    // INT VariantTimeToSystemTime(DOUBLE vtime, LPSYSTEMTIME lpSystemTime)
    // https://learn.microsoft.com/en-us/windows/win32/api/oleauto/nf-oleauto-varianttimetosystemtime
    // The DATE is a double: XMM0 on x64 (the pointer is then in RDX, arg 1),
    // two stack slots on x86 (the pointer is arg 2). Measured
    // (tools/oracle_probe/s7_ole_probe.expected): the time rounds to the
    // nearest second with wMilliseconds 0; a negative date's time of day is
    // its |fraction| (-1.25 = 1899-12-29 06:00); the valid range is
    // -657434.0 (100-01-01) .. 2958465.99999 (9999-12-31 23:59:59); out of
    // range is FALSE with the SYSTEMTIME untouched; wDayOfWeek is filled.
    exports.entries.push_back(ExportEntry{ "VariantTimeToSystemTime", 185,
        [](Marshaler& m){
            const bool is64 = m.cpu_is_64bit();
            u64 bits = 0;
            if (is64 && m.cpu() != nullptr) bits = m.cpu()->sse.xmm[0].u64_[0];
            else bits = (m.arg(0) & 0xFFFFFFFFull) | (m.arg(1) << 32);
            double date = 0;
            std::memcpy(&date, &bits, sizeof(date));
            const GuestAddr out = m.arg_ptr(is64 ? 1 : 2);
            constexpr double kMinDate = -657434.0;
            constexpr double kMaxDate = 2958466.0;
            if (out == 0 || !(date >= kMinDate && date < kMaxDate)) {
                m.set_return(0);
                return;
            }
            double whole = std::trunc(date);
            double fraction = std::fabs(date - whole);
            i64 days = static_cast<i64>(whole);
            i64 seconds = static_cast<i64>(std::llround(fraction * 86400.0));
            if (seconds >= 86400) {
                seconds -= 86400;
                days += (date < 0) ? -1 : 1;
            }
            // Days since 1899-12-30 -> civil date (proleptic Gregorian).
            const i64 z = days + 693899;  // civil_from_days epoch (0000-03-01)
            const i64 era = (z >= 0 ? z : z - 146096) / 146097;
            const i64 doe = z - era * 146097;
            const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const i64 mp = (5 * doy + 2) / 153;
            const i64 day = doy - (153 * mp + 2) / 5 + 1;
            const i64 month = mp < 10 ? mp + 3 : mp - 9;
            const i64 year = yoe + era * 400 + (month <= 2 ? 1 : 0);
            // 1899-12-30 was a Saturday (6).
            const i64 dow = ((days % 7) + 7 + 6) % 7;
            const u16 fields[8] = {
                static_cast<u16>(year), static_cast<u16>(month), static_cast<u16>(dow),
                static_cast<u16>(day), static_cast<u16>(seconds / 3600),
                static_cast<u16>((seconds / 60) % 60), static_cast<u16>(seconds % 60), 0};
            for (u32 i = 0; i < 8; ++i) (void)m.write_u16(out + i * 2, fields[i]);
            m.set_return(1);
        }, 12 });

    // ------------------------------------------------------------------------
    // Ordinal-only oleaut32 exports. An unresolved export becomes a
    // fail-thunk returning 0xFFFFFFFF, which deadlocks init on the loader
    // lock. See the implementations above.
    // ------------------------------------------------------------------------
    // #162 LoadRegTypeLib(REFGUID, WORD, WORD, LCID, ITypeLib**)
    exports.entries.push_back(ExportEntry{ "LoadRegTypeLib", 162,
        [&mgr](Marshaler& m){ impl_LoadRegTypeLib(m, mgr); }, 20 });
    // #277 VarUI4FromStr(OLECHAR*, LCID, ULONG, ULONG*)
    exports.entries.push_back(ExportEntry{ "VarUI4FromStr", 277,
        [](Marshaler& m){ impl_VarUI4FromStr(m); }, 16 });
    // BSTR NDR user-marshal quartet — ordinals from NT 5.1–10 oleaut32 tables.
    // #283 BSTR_UserSize(ULONG*, ULONG, BSTR*) -> ULONG, 12 stdcall bytes
    exports.entries.push_back(ExportEntry{ "BSTR_UserSize", 283,
        [](Marshaler& m){ impl_BSTR_UserSize(m); }, 12 });
    // #284 BSTR_UserMarshal(ULONG*, unsigned char*, BSTR*) -> unsigned char*
    exports.entries.push_back(ExportEntry{ "BSTR_UserMarshal", 284,
        [](Marshaler& m){ impl_BSTR_UserMarshal(m); }, 12 });
    // #285 BSTR_UserUnmarshal(ULONG*, unsigned char*, BSTR*) -> unsigned char*
    exports.entries.push_back(ExportEntry{ "BSTR_UserUnmarshal", 285,
        [&mgr](Marshaler& m){ impl_BSTR_UserUnmarshal(m, mgr); }, 12 });
    // #286 BSTR_UserFree(ULONG*, BSTR*) — void stdcall, 8 bytes on x86.
    exports.entries.push_back(ExportEntry{ "BSTR_UserFree", 286,
        [&mgr](Marshaler& m){ impl_BSTR_UserFree(m, mgr); }, 8 });
    // VARIANT NDR user-marshal quartet (#287–#290).
    exports.entries.push_back(ExportEntry{ "VARIANT_UserSize", 287,
        [](Marshaler& m){ impl_VARIANT_UserSize(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "VARIANT_UserMarshal", 288,
        [](Marshaler& m){ impl_VARIANT_UserMarshal(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "VARIANT_UserUnmarshal", 289,
        [](Marshaler& m){ impl_VARIANT_UserUnmarshal(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "VARIANT_UserFree", 290,
        [&mgr](Marshaler& m){ impl_VARIANT_UserFree(m, mgr); }, 8 });
    // LPSAFEARRAY NDR user-marshal quartet (#291–#294).
    exports.entries.push_back(ExportEntry{ "LPSAFEARRAY_UserSize", 291,
        [](Marshaler& m){ impl_LPSAFEARRAY_UserSize(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "LPSAFEARRAY_UserMarshal", 292,
        [](Marshaler& m){ impl_LPSAFEARRAY_UserMarshal(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "LPSAFEARRAY_UserUnmarshal", 293,
        [](Marshaler& m){ impl_LPSAFEARRAY_UserUnmarshal(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "LPSAFEARRAY_UserFree", 294,
        [&mgr](Marshaler& m){ impl_LPSAFEARRAY_UserFree(m, mgr); }, 8 });
    // #313 VarBstrCat(BSTR, BSTR, LPBSTR) — stdcall 12 on x86.
    exports.entries.push_back(ExportEntry{ "VarBstrCat", 313,
        [&mgr](Marshaler& m){ impl_VarBstrCat(m, mgr); }, 12 });
    // #314 VarBstrCmp(BSTR, BSTR, LCID, ULONG)
    exports.entries.push_back(ExportEntry{ "VarBstrCmp", 314,
        [](Marshaler& m){ impl_VarBstrCmp(m); }, 16 });
    // #411 SafeArrayCreateVector(VARTYPE, LONG, ULONG)
    exports.entries.push_back(ExportEntry{ "SafeArrayCreateVector", 411,
        [&mgr](Marshaler& m){ impl_SafeArrayCreateVector(m, mgr); }, 12 });

    // ------------------------------------------------------------------------
    // Type-library APIs beyond LoadTypeLib/RegisterTypeLib. Ordinals measured
    // on Windows 11 x86 and x64 (automation_export_ordinals_probe.*.expected:
    // LoadTypeLibEx 183, UnRegisterTypeLib 186); importers by ordinal exist
    // (the .NET Framework 4 setup's type-library custom action).
    // ------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{ "LoadTypeLibEx", 183,
        [&mgr](Marshaler& m){ m.set_return(mgr.load_type_lib_ex(m)); }, 12 });
    exports.entries.push_back(ExportEntry{ "UnRegisterTypeLib", 186,
        [&mgr](Marshaler& m){ m.set_return(mgr.unregister_type_lib(m)); }, 20 });

    // ------------------------------------------------------------------------
    // ITypeLib vtable slots for the objects LoadTypeLib hands back. Every slot
    // is filled so a guest can call any of them; the ones Vitreo cannot answer
    // from the library header fail with the documented HRESULT.
    // ------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_QueryInterface", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_AddRef", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_Release", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_GetTypeInfoCount", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_get_type_info_count(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_GetTypeInfo", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_get_type_info(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_GetTypeInfoType", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_get_type_info_type(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_GetTypeInfoOfGuid", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_get_type_info_of_guid(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_GetLibAttr", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_get_lib_attr(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_GetTypeComp", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_get_type_comp(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_GetDocumentation", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_get_documentation(m)); }, 24});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_IsName", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_is_name(m)); }, 16});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_FindName", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_find_name(m)); }, 24});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeLib_ReleaseTLibAttr", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typelib_release_tlib_attr(m)); }, 8});

    // ------------------------------------------------------------------------
    // ITypeInfo vtable slots. Every slot is filled so a guest can call any of
    // them; methods needing decoded FUNCDESC/VARDESC fail honestly.
    // ------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_QueryInterface", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_query_interface(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_AddRef", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_Release", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_release(m)); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetTypeAttr", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_type_attr(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetTypeComp", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_type_comp(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetFuncDesc", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_func_desc(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetVarDesc", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_var_desc(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetNames", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_names(m)); }, 20});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetRefTypeOfImplType", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_ref_type_of_impl_type(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetImplTypeFlags", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_impl_type_flags(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetIDsOfNames", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_ids_of_names(m)); }, 16});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_Invoke", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_invoke(m)); }, 32});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetDocumentation", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_documentation(m)); }, 24});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetDllEntry", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_dll_entry(m)); }, 24});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetRefTypeInfo", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_ref_type_info(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_AddressOfMember", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_address_of_member(m)); }, 16});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_CreateInstance", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_create_instance(m)); }, 16});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetMops", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_mops(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_GetContainingTypeLib", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_get_containing_type_lib(m)); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_ReleaseTypeAttr", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_release_type_attr(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_ReleaseFuncDesc", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_release_func_desc(m)); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_TypeInfo_ReleaseVarDesc", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.typeinfo_release_var_desc(m)); }, 8});

    return exports;
}

} // namespace vitreo::winapi
