// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_core_thunks_ole.cpp
/// @brief Public OLE32 exports: GUID/string conversion and the additional OLE32
/// stubs (ROT, monikers, ProgID, class-object registration, marshal helpers,
/// security, PropVariantClear).

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_thunks_internal.hpp"

#include <array>
#include <string>

namespace vitreo::winapi {

namespace {

[[nodiscard]] bool write_utf16le_bounded(GuestMemory& mem,
                                        GuestAddr dst,
                                        std::string_view ascii,
                                        u32 cch_max) {
    // cch_max is in UTF-16 code units.
    const u32 needed = static_cast<u32>(ascii.size() + 1);
    if (dst == 0 || cch_max < needed) {
        return false;
    }

    for (usize i = 0; i < ascii.size(); ++i) {
        const u16 ch = static_cast<u16>(static_cast<unsigned char>(ascii[i]));
        auto wr = mem.write_bytes(dst + static_cast<GuestAddr>(i * 2),
                                  reinterpret_cast<const Byte*>(&ch),
                                  sizeof(ch));
        if (wr.is_err()) {
            return false;
        }
    }
    const u16 nul = 0;
    auto wrnul = mem.write_bytes(dst + static_cast<GuestAddr>(ascii.size() * 2),
                                 reinterpret_cast<const Byte*>(&nul),
                                 sizeof(nul));
    return !wrnul.is_err();
}

} // namespace

/// CLSIDFromString, IIDFromString, StringFromGUID2, StringFromCLSID and
/// StringFromIID.
void append_com_guid_string_thunks(DllExports& exports, ComCoreManager& mgr) {
    // HRESULT CLSIDFromString(LPCOLESTR lpsz, LPCLSID pclsid)
    // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-clsidfromstring
    // Measured (tools/oracle_probe/s7_ole_probe.expected):
    //  - NULL lpsz -> S_OK with GUID_NULL written.
    //  - A string starting with '{' is parsed strictly as
    //    {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} followed by the terminator.
    //    Parsing writes as it goes: Data1 is stored even when a non-hex digit
    //    stops it, Data2/Data3 only when all four digits are valid, and each
    //    Data4 byte when both of its digits are. Anything malformed is
    //    CO_E_CLASSSTRING with that partial GUID left behind.
    //  - Any other string is a ProgID (HKCR\<ProgID>\CLSID); an unknown one
    //    is CO_E_CLASSSTRING with the output zeroed.
    exports.entries.push_back(ExportEntry{
        "CLSIDFromString", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr lpsz = m.arg_ptr(0);
            const GuestAddr pclsid = m.arg_ptr(1);
            if (pclsid == 0) {
                m.set_return(com::E_INVALIDARG);
                return;
            }
            Guid g{};
            if (lpsz == 0) {
                m.set_return(m.write_struct<Guid>(pclsid, g).is_ok()
                                 ? com::S_OK : com::E_INVALIDARG);
                return;
            }
            std::u16string text;
            for (GuestAddr at = lpsz; text.size() < 1024; at += 2) {
                const u16 ch = m.read_u16(at).value_or(0);
                if (ch == 0) break;
                text.push_back(static_cast<char16_t>(ch));
            }
            if (!text.empty() && text[0] == u'{') {
                usize pos = 1;
                auto hex_digit = [&](usize at, u32& digit) {
                    if (at >= text.size()) return false;
                    const char16_t c = text[at];
                    if (c >= u'0' && c <= u'9') { digit = c - u'0'; return true; }
                    if (c >= u'a' && c <= u'f') { digit = c - u'a' + 10; return true; }
                    if (c >= u'A' && c <= u'F') { digit = c - u'A' + 10; return true; }
                    return false;
                };
                auto fail = [&]() {
                    (void)m.write_struct<Guid>(pclsid, g);
                    m.set_return(com::CO_E_CLASSSTRING);
                };
                auto dash = [&]() { return pos < text.size() && text[pos++] == u'-'; };
                // Start from the caller's bytes so unparsed fields stay as-is.
                if (auto current = m.read_struct<Guid>(pclsid); current.is_ok()) {
                    g = current.value();
                }
                u32 d1 = 0, digit = 0;
                for (int i = 0; i < 8; ++i, ++pos) {
                    if (!hex_digit(pos, digit)) { g.Data1 = d1; fail(); return; }
                    d1 = (d1 << 4) | digit;
                }
                g.Data1 = d1;
                auto word = [&](u16& field) {
                    u32 v = 0;
                    for (int i = 0; i < 4; ++i, ++pos) {
                        if (!hex_digit(pos, digit)) return false;
                        v = (v << 4) | digit;
                    }
                    field = static_cast<u16>(v);
                    return true;
                };
                auto byte = [&](u8& field) {
                    u32 hi = 0, lo = 0;
                    if (!hex_digit(pos, hi) || !hex_digit(pos + 1, lo)) return false;
                    pos += 2;
                    field = static_cast<u8>((hi << 4) | lo);
                    return true;
                };
                if (!dash() || !word(g.Data2) || !dash() || !word(g.Data3) ||
                    !dash() || !byte(g.Data4[0]) || !byte(g.Data4[1]) || !dash()) {
                    fail();
                    return;
                }
                for (int i = 2; i < 8; ++i) {
                    if (!byte(g.Data4[i])) { fail(); return; }
                }
                if (pos + 1 != text.size() || text[pos] != u'}') { fail(); return; }
                m.set_return(m.write_struct<Guid>(pclsid, g).is_ok()
                                 ? com::S_OK : com::E_INVALIDARG);
                return;
            }
            // ProgID form.
            const u32 hr = mgr.clsid_from_progid_text(text, g);
            if (hr != com::S_OK) g = Guid{};
            (void)m.write_struct<Guid>(pclsid, g);
            m.set_return(hr == com::S_OK ? com::S_OK : com::CO_E_CLASSSTRING);
        },
        8
    });

    // IIDFromString (2 args, stdcall_bytes=8)
    // Same braced-GUID parse as CLSIDFromString. NULL lpsz yields GUID_NULL
    // per Microsoft Learn for IIDFromString.
    exports.entries.push_back(ExportEntry{
        "IIDFromString", 0,
        [](Marshaler& m) {
            const GuestAddr lpsz = m.arg_ptr(0);
            const GuestAddr lpiid = m.arg_ptr(1);
            if (lpiid == 0) {
                m.set_return(com::E_INVALIDARG);
                return;
            }

            Guid g{};
            if (lpsz == 0) {
                auto wr = m.write_struct<Guid>(lpiid, g);
                m.set_return(wr.is_ok() ? com::S_OK : com::E_INVALIDARG);
                return;
            }

            auto str_res = m.read_wide_string(lpsz);
            if (str_res.is_err()) {
                m.set_return(com::E_INVALIDARG);
                return;
            }

            if (!parse_guid_string(str_res.value(), g)) {
                // Windows 11 returns CO_E_IIDSTRING here (s7_ole_probe,
                // x86 and x64); CLSIDFromString returns CO_E_CLASSSTRING.
                m.set_return(0x800401F4u);  // CO_E_IIDSTRING
                return;
            }

            auto wr = m.write_struct<Guid>(lpiid, g);
            if (wr.is_err()) {
                m.set_return(com::E_INVALIDARG);
                return;
            }
            m.set_return(com::S_OK);
        },
        8
    });

    // StringFromGUID2 (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "StringFromGUID2", 0,
        [](Marshaler& m) {
            const GuestAddr rguid = m.arg_ptr(0);
            const GuestAddr lpsz = m.arg_ptr(1);
            // cchMax is an int: a negative size is "too small" (measured 0).
            const i32 cch_signed = static_cast<i32>(m.arg32(2));
            const u32 cch_max = cch_signed < 0 ? 0u : static_cast<u32>(cch_signed);

            if (rguid == 0 || lpsz == 0 || cch_max == 0) {
                m.set_return(0);
                return;
            }

            auto guid_res = m.read_struct<Guid>(rguid);
            if (guid_res.is_err()) {
                m.set_return(0);
                return;
            }

            const std::string s = format_guid(guid_res.value());
            if (!write_utf16le_bounded(m.memory(), lpsz, s, cch_max)) {
                m.set_return(0);
                return;
            }

            // Return number of UTF-16 characters written, including NUL.
            m.set_return(static_cast<u64>(s.size() + 1));
        },
        12
    });

    // HRESULT StringFromCLSID(REFCLSID rclsid, LPOLESTR *lplpsz)
    //
    // Allocates the braced text form with CoTaskMemAlloc and hands the caller
    // ownership. A missing-export stub returns 0xFFFFFFFF without writing the
    // out-parameter. ATL servers throw on that HRESULT, unwind, and report
    // E_UNEXPECTED to their caller.
    const auto string_from_guid_alloc = [&mgr](Marshaler& m) {
            const GuestAddr rclsid = m.arg_ptr(0);
            const GuestAddr out    = m.arg_ptr(1);
            if (out == 0) {
                m.set_return(com::E_INVALIDARG);
                return;
            }
            (void)m.write_ptr(out, 0);
            if (rclsid == 0) {
                m.set_return(com::E_INVALIDARG);
                return;
            }

            auto guid = m.read_struct<Guid>(rclsid);
            if (guid.is_err()) {
                m.set_return(com::E_INVALIDARG);
                return;
            }
            const std::string text = format_guid(guid.value());

            // 38 characters plus the NUL, as UTF-16.
            const u32 bytes = static_cast<u32>((text.size() + 1) * 2);
            const GuestAddr buffer = mgr.co_task_mem_alloc_guest(bytes);
            if (buffer == 0) {
                m.set_return(0x8007000Eu); // E_OUTOFMEMORY
                return;
            }
            for (usize i = 0; i < text.size(); ++i) {
                (void)m.write_u16(buffer + static_cast<GuestAddr>(i) * 2,
                                  static_cast<u16>(
                                      static_cast<unsigned char>(text[i])));
            }
            (void)m.write_u16(buffer + static_cast<GuestAddr>(text.size()) * 2, 0);

            if (m.write_ptr(out, buffer).is_err()) {
                m.set_return(com::E_INVALIDARG);
                return;
            }
            VITREO_TRACE("[ole32] StringFromCLSID -> \"%s\" at 0x%08x\n",
                         text.c_str(), static_cast<unsigned>(buffer));
            m.set_return(com::S_OK);
    };
    exports.entries.push_back(ExportEntry{
        "StringFromCLSID", 0, string_from_guid_alloc, 8});
    // HRESULT StringFromIID(REFIID rclsid, LPOLESTR *lplpsz): the same
    // CoTaskMem-allocated braced text as StringFromCLSID (Learn: S_OK or
    // E_OUTOFMEMORY). Missing, it bound to the stub that returns 0xFFFFFFFF
    // without writing *lplpsz.
    exports.entries.push_back(ExportEntry{
        "StringFromIID", 0, string_from_guid_alloc, 8});
}

/// Additional OLE32 exports: ROT, monikers, ProgID lookup, class-object
/// registration, marshal helpers, security and PropVariantClear.
void append_com_ole_stub_thunks(DllExports& exports, ComCoreManager& mgr) {
    // --- Additional OLE32 stubs ---
    exports.entries.push_back(ExportEntry{
        "IsAccelerator", 0,
        [&mgr](Marshaler& m) { mgr.impl_IsAccelerator(m); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "OleTranslateAccelerator", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleTranslateAccelerator(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "OleDestroyMenuDescriptor", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleDestroyMenuDescriptor(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "OleCreateMenuDescriptor", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleCreateMenuDescriptor(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "OleLockRunning", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleLockRunning(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "OleRun", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleRun(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CoWaitForMultipleHandles", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoWaitForMultipleHandles(m); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "OleGetClipboard", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleGetClipboard(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "OleSetClipboard", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleSetClipboard(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "OleIsCurrentClipboard", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleIsCurrentClipboard(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "OleFlushClipboard", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleFlushClipboard(m); },
        0
    });
    exports.entries.push_back(ExportEntry{
        "RevokeDragDrop", 0,
        [&mgr](Marshaler& m) { mgr.impl_RevokeDragDrop(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CoLockObjectExternal", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoLockObjectExternal(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "RegisterDragDrop", 0,
        [&mgr](Marshaler& m) { mgr.impl_RegisterDragDrop(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "DoDragDrop", 0,
        [&mgr](Marshaler& m) { mgr.impl_DoDragDrop(m); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "CreateStreamOnHGlobal", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateStreamOnHGlobal(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "WriteClassStm", 0,
        [&mgr](Marshaler& m) { mgr.impl_WriteClassStm(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "OleSaveToStream", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleSaveToStream(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "OleLoadFromStream", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleLoadFromStream(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetRunningObjectTable", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetRunningObjectTable(m); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ROT_QueryInterface", 0,
        [&mgr](Marshaler& m) { mgr.running_object_table_query_interface(m); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ROT_AddRef", 0,
        [&mgr](Marshaler& m) { mgr.running_object_table_add_ref(m); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_ROT_Release", 0,
        [&mgr](Marshaler& m) { mgr.running_object_table_release(m); }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_ROT_Register", 0,
        [&mgr](Marshaler& m) { mgr.running_object_table_register(m); }, 20});
    exports.entries.push_back(ExportEntry{"Vitreo_ROT_Revoke", 0,
        [&mgr](Marshaler& m) { mgr.running_object_table_revoke(m); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ROT_IsRunning", 0,
        [&mgr](Marshaler& m) { mgr.running_object_table_is_running(m); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_ROT_GetObject", 0,
        [&mgr](Marshaler& m) { mgr.running_object_table_get_object(m); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ROT_NoteChangeTime", 0,
        [&mgr](Marshaler& m) { mgr.running_object_table_note_change_time(m); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ROT_GetTimeOfLastChange", 0,
        [&mgr](Marshaler& m) { mgr.running_object_table_get_time_of_last_change(m); }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_ROT_EnumRunning", 0,
        [&mgr](Marshaler& m) { mgr.running_object_table_enum_running(m); }, 8});
    exports.entries.push_back(ExportEntry{"CreateItemMoniker", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateItemMoniker(m); }, 12});
    static constexpr std::array<std::pair<std::string_view, u32>, 11>
        item_moniker_methods{{
            {"Vitreo_ItemMoniker_QueryInterface", 0},
            {"Vitreo_ItemMoniker_AddRef", 1},
            {"Vitreo_ItemMoniker_Release", 2},
            {"Vitreo_ItemMoniker_GetClassID", 3},
            {"Vitreo_ItemMoniker_IsDirty", 4},
            {"Vitreo_ItemMoniker_IsEqual", 13},
            {"Vitreo_ItemMoniker_Hash", 14},
            {"Vitreo_ItemMoniker_GetDisplayName", 20},
            {"Vitreo_ItemMoniker_IsSystemMoniker", 22},
            {"Vitreo_ItemMoniker_NotImpl1", 5},
            {"Vitreo_ItemMoniker_NotImpl2", 5}}};
    static constexpr std::array<u16, 11> item_method_bytes{{
        12, 4, 4, 8, 4, 8, 8, 16, 8, 8, 12}};
    for (usize i = 0; i < item_moniker_methods.size(); ++i) {
        const auto [name, slot] = item_moniker_methods[i];
        exports.entries.push_back(ExportEntry{std::string(name), 0,
            [&mgr, slot](Marshaler& m) { mgr.item_moniker_method(m, slot); },
            item_method_bytes[i]});
    }
    exports.entries.push_back(ExportEntry{"Vitreo_ItemMoniker_NotImpl3", 0,
        [&mgr](Marshaler& m) { mgr.item_moniker_method(m, 5); }, 16});
    exports.entries.push_back(ExportEntry{"Vitreo_ItemMoniker_NotImpl4", 0,
        [&mgr](Marshaler& m) { mgr.item_moniker_method(m, 5); }, 20});
    exports.entries.push_back(ExportEntry{"Vitreo_ItemMoniker_NotImpl5", 0,
        [&mgr](Marshaler& m) { mgr.item_moniker_method(m, 5); }, 24});
    exports.entries.push_back(ExportEntry{"ProgIDFromCLSID", 0,
        [&mgr](Marshaler& m) { mgr.impl_ProgIDFromCLSID(m); }, 8});
    exports.entries.push_back(ExportEntry{"CLSIDFromProgID", 0,
        [&mgr](Marshaler& m) { mgr.impl_CLSIDFromProgID(m); }, 8});
    exports.entries.push_back(ExportEntry{"CoRegisterClassObject", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoRegisterClassObject(m); }, 20});
    exports.entries.push_back(ExportEntry{"CoRevokeClassObject", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoRevokeClassObject(m); }, 4});
    exports.entries.push_back(ExportEntry{"CoRegisterPSClsid", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoRegisterPSClsid(m); }, 8});
    exports.entries.push_back(ExportEntry{"CoGetPSClsid", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoGetPSClsid(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "CoMarshalInterThreadInterfaceInStream", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_CoMarshalInterThreadInterfaceInStream(m);
        },
        12
    });
    exports.entries.push_back(ExportEntry{
        "CoGetInterfaceAndReleaseStream", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoGetInterfaceAndReleaseStream(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "CoReleaseMarshalData", 0,
        [&mgr](Marshaler& m) { mgr.impl_CoReleaseMarshalData(m); }, 4});
    exports.entries.push_back(ExportEntry{
        "OleDuplicateData", 0,
        [&mgr](Marshaler& m) { mgr.impl_OleDuplicateData(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "ReleaseStgMedium", 0,
        [&mgr](Marshaler& m) { mgr.impl_ReleaseStgMedium(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CoInitializeSecurity", 0,
        [&mgr](Marshaler& m) {
            // All-zero args -> E_INVALIDARG (0x80070057). Real Windows requires
            // either a SECURITY_DESCRIPTOR or app-id pointer.
            if (m.arg(0) == 0 && m.arg(1) == 0 && m.arg(2) == 0 && m.arg(3) == 0) {
                m.set_return(0x80070057u);
                return;
            }
            mgr.impl_CoInitializeSecurity(m);
        },
        36
    });
    exports.entries.push_back(ExportEntry{
        "CoSetProxyBlanket", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0x80070057u); return; }
            mgr.impl_CoSetProxyBlanket(m);
        },
        32
    });

    // HRESULT PropVariantClear(PROPVARIANT*): frees the payload per vt and
    // zeroes the 16 (x86) / 24 (x64) byte structure (prop_variant_clear_at).
    exports.entries.push_back(ExportEntry{
        "PropVariantClear", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.prop_variant_clear_at(m, m.arg_ptr(0)));
        },
        4
    });
}

} // namespace vitreo::winapi
