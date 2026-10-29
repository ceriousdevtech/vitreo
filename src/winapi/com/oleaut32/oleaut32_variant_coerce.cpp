// SPDX-License-Identifier: LGPL-2.1-only
/// @file oleaut32_variant_coerce.cpp
/// @brief OLEAUT32 VARIANT coercion: VariantChangeType(Ex) and DispGetParam.
///
/// Split from oleaut32.cpp by responsibility.

#include "oleaut32.hpp"
#include "winapi/com/oleaut32/oleaut32_internal2.hpp"
#include "winapi/com/oleaut32/oleaut32_internal.hpp"
#include "winapi/com/oleaut32/oleaut32_shared_internal.hpp"
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

u32 OleAut32Manager::variant_change_type(Marshaler& m) {
    return variant_change_type_ex(m, m.arg_ptr(0), m.arg_ptr(1),
                                  static_cast<u16>(m.arg(2)),
                                  static_cast<u16>(m.arg(3)));
}

u32 OleAut32Manager::variant_change_type_ex(Marshaler& m,
                                            GuestAddr pvarg_dest,
                                            GuestAddr pvar_src,
                                            u16 /*wFlags*/,
                                            u16 target_vt) {
    const u32 vsz = oleaut_variant_size(m.cpu_is_64bit());
    u8 src_buf[24] = {};
    (void)m.memory().read_bytes(pvar_src, src_buf, vsz);
    u16 src_vt = static_cast<u16>(src_buf[0] | (src_buf[1] << 8));

    // Read source value from offset 8
    auto read_src_i32 = [&]() -> i32 {
        return static_cast<i32>(src_buf[8] | (src_buf[9] << 8) |
               (src_buf[10] << 16) | (src_buf[11] << 24));
    };
    auto read_src_u32 = [&]() -> u32 {
        return src_buf[8] | (src_buf[9] << 8) |
               (src_buf[10] << 16) | (src_buf[11] << 24);
    };
    auto read_src_i16 = [&]() -> i16 {
        return static_cast<i16>(src_buf[8] | (src_buf[9] << 8));
    };
    auto read_src_r8 = [&]() -> double {
        double v;
        std::memcpy(&v, &src_buf[8], 8);
        return v;
    };
    auto read_src_r4 = [&]() -> float {
        float v;
        std::memcpy(&v, &src_buf[8], 4);
        return v;
    };

    VITREO_TRACE("[oleaut32] VariantChangeType dest=0x%x src=0x%x srcVT=%u targetVT=%u\n",
                 (unsigned)pvarg_dest, (unsigned)pvar_src, src_vt, target_vt);

    // Same type → just copy
    if (src_vt == target_vt) {
        (void)m.memory().write_bytes(pvarg_dest, src_buf, vsz);
        return S_OK;
    }

    // VT_BYREF|T → dereference, then coerce the pointed-to value.
    // Script engines pass [in,out] longs as VT_BYREF|VT_I4.
    if ((src_vt & VT_BYREF) != 0) {
        const u16 base_vt = static_cast<u16>(src_vt & VT_TYPEMASK);
        const GuestAddr ptr =
            static_cast<GuestAddr>(m.read_ptr(pvar_src + 8).value_or(0));
        if (ptr == 0) {
            VITREO_TRACE("[oleaut32]   -> DISP_E_TYPEMISMATCH (null BYREF)\n");
            return DISP_E_TYPEMISMATCH;
        }
        if (base_vt == VT_VARIANT) {
            return variant_change_type_ex(m, pvarg_dest, ptr, 0, target_vt);
        }
        // Materialise a non-BYREF VARIANT in scratch and recurse once.
        const GuestAddr tmp = scratch_alloc(vsz);
        if (tmp == 0) return 0x8007000Eu; // E_OUTOFMEMORY
        u8 tmp_buf[24]{};
        tmp_buf[0] = static_cast<u8>(base_vt & 0xFF);
        tmp_buf[1] = static_cast<u8>(base_vt >> 8);
        if (base_vt == VT_UI1 || base_vt == VT_I1) {
            // Single-byte types: only the low byte is meaningful.
            tmp_buf[8] = m.read_u8(ptr).value_or(0);
        } else if (base_vt == VT_I2 || base_vt == VT_BOOL || base_vt == VT_UI2) {
            const u16 v = m.read_u16(ptr).value_or(0);
            tmp_buf[8] = static_cast<u8>(v & 0xFF);
            tmp_buf[9] = static_cast<u8>(v >> 8);
        } else if (base_vt == VT_R8) {
            u8 eight[8]{};
            (void)m.memory().read_bytes(ptr, eight, 8);
            std::memcpy(&tmp_buf[8], eight, 8);
        } else if (base_vt == VT_BSTR || base_vt == VT_UNKNOWN ||
                   base_vt == VT_DISPATCH) {
            (void)m.memory().write_bytes(tmp, tmp_buf, vsz);
            (void)m.write_u16(tmp, base_vt);
            (void)m.write_ptr(tmp + 8, static_cast<GuestAddr>(
                m.read_ptr(ptr).value_or(0)));
            return variant_change_type_ex(m, pvarg_dest, tmp, 0, target_vt);
        } else if (base_vt == VT_I4 || base_vt == VT_UI4 || base_vt == VT_R4) {
            const u32 v = m.read_u32(ptr).value_or(0);
            tmp_buf[8]  = static_cast<u8>(v);
            tmp_buf[9]  = static_cast<u8>(v >> 8);
            tmp_buf[10] = static_cast<u8>(v >> 16);
            tmp_buf[11] = static_cast<u8>(v >> 24);
        } else {
            VITREO_TRACE("[oleaut32]   -> DISP_E_TYPEMISMATCH (BYREF base=%u)\n",
                         base_vt);
            return DISP_E_TYPEMISMATCH;
        }
        (void)m.memory().write_bytes(tmp, tmp_buf, vsz);
        return variant_change_type_ex(m, pvarg_dest, tmp, 0, target_vt);
    }

    // Prepare destination: zero it, set target vt
    u8 dst_buf[24] = {};
    dst_buf[0] = static_cast<u8>(target_vt & 0xFF);
    dst_buf[1] = static_cast<u8>(target_vt >> 8);

    auto write_dst_i32 = [&](i32 v) {
        dst_buf[8]  = static_cast<u8>(v);
        dst_buf[9]  = static_cast<u8>(v >> 8);
        dst_buf[10] = static_cast<u8>(v >> 16);
        dst_buf[11] = static_cast<u8>(v >> 24);
    };
    auto write_dst_u32 = [&](u32 v) {
        dst_buf[8]  = static_cast<u8>(v);
        dst_buf[9]  = static_cast<u8>(v >> 8);
        dst_buf[10] = static_cast<u8>(v >> 16);
        dst_buf[11] = static_cast<u8>(v >> 24);
    };
    auto write_dst_r8 = [&](double v) {
        std::memcpy(&dst_buf[8], &v, 8);
    };
    auto write_dst_i16 = [&](i16 v) {
        dst_buf[8] = static_cast<u8>(v & 0xFF);
        dst_buf[9] = static_cast<u8>((v >> 8) & 0xFF);
    };

    // Helper: read BSTR content as UTF-8 from a guest BSTR pointer
    auto read_bstr_utf8 = [&](GuestAddr bstr_addr) -> std::string {
        if (bstr_addr == 0) return {};
        u32 byte_len = m.read_u32(bstr_addr - 4).value_or(0);
        if (byte_len == 0) return {};
        std::string result;
        for (u32 i = 0; i < byte_len; i += 2) {
            u16 ch = m.read_u16(bstr_addr + i).value_or(0);
            if (ch < 0x80) result += static_cast<char>(ch);
        }
        return result;
    };

    // Helper: allocate BSTR from a narrow string
    auto alloc_bstr_from_str = [&](const std::string& s) -> GuestAddr {
        std::vector<Byte> utf16(s.size() * 2);
        for (usize i = 0; i < s.size(); ++i) {
            utf16[i * 2] = static_cast<Byte>(static_cast<u8>(s[i]));
        }
        return allocate_bstr_bytes(m, utf16);
    };
    // Pointer payloads (BSTR, interface pointers) are pointer-sized.
    auto write_dst_ptr = [&](GuestAddr v) {
        const u32 n = oleaut_ptr_size(m.cpu_is_64bit());
        for (u32 i = 0; i < n; ++i) {
            dst_buf[8 + i] = static_cast<u8>(v >> (8 * i));
        }
    };

    bool ok = false;

    switch (src_vt) {
    case VT_BSTR: {
        GuestAddr bstr_addr = 0;
        std::memcpy(&bstr_addr, &src_buf[8], oleaut_ptr_size(m.cpu_is_64bit()));
        std::string str = read_bstr_utf8(bstr_addr);
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            char* end = nullptr;
            long val = std::strtol(str.c_str(), &end, 10);
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_R8) {
            char* end = nullptr;
            double val = std::strtod(str.c_str(), &end);
            write_dst_r8(val);
            ok = true;
        } else if (target_vt == VT_BOOL) {
            char* end = nullptr;
            long val = std::strtol(str.c_str(), &end, 10);
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        }
        break;
    }
    case VT_I4:
    case VT_UI4: {
        i32 val = read_src_i32();
        if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_R4) {
            float f = static_cast<float>(val);
            std::memcpy(&dst_buf[8], &f, 4);
            ok = true;
        } else if (target_vt == VT_BSTR) {
            std::string s = std::to_string(val);
            const GuestAddr bstr = alloc_bstr_from_str(s);
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        } else if (target_vt == VT_I2) {
            write_dst_i16(static_cast<i16>(val));
            ok = true;
        }
        break;
    }
    case VT_R8: {
        double val = read_src_r8();
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_BSTR) {
            std::string s = std::to_string(val);
            const GuestAddr bstr = alloc_bstr_from_str(s);
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0.0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        }
        break;
    }
    case VT_R4: {
        float val = read_src_r4();
        if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_I4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        }
        break;
    }
    case VT_BOOL: {
        i16 val = read_src_i16();
        if (target_vt == VT_I2) {
            // VARIANT_BOOL is already an i16; script engines (ISRT) coerce
            // BOOL→I2 when reading typed locals.
            write_dst_i16(val);
            ok = true;
        } else if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(val != 0 ? 1 : 0);
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(val != 0 ? 1.0 : 0.0);
            ok = true;
        } else if (target_vt == VT_BSTR) {
            // Check VARIANT_ALPHABOOL (0x02) in wFlags
            u16 wFlags = static_cast<u16>(m.arg(2));
            std::string s;
            if (wFlags & 0x02) {
                // VARIANT_ALPHABOOL: use "True"/"False"
                s = val != 0 ? "True" : "False";
            } else {
                // Default OLE convention: "#TRUE#"/"#FALSE#"
                s = val != 0 ? "#TRUE#" : "#FALSE#";
            }
            const GuestAddr bstr = alloc_bstr_from_str(s);
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        }
        break;
    }
    case VT_I2: {
        i16 val = read_src_i16();
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        }
        break;
    }
    case VT_UI1: {
        // Unsigned byte — always fits in I2/I4/UI4/R*/BOOL/BSTR.
        // Script engines coerce typed locals UI1→I4.
        const u8 val = src_buf[8];
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_I2 || target_vt == VT_UI2) {
            write_dst_i16(static_cast<i16>(val));
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_R4) {
            float f = static_cast<float>(val);
            std::memcpy(&dst_buf[8], &f, 4);
            ok = true;
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        } else if (target_vt == VT_BSTR) {
            const GuestAddr bstr = alloc_bstr_from_str(std::to_string(val));
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        } else if (target_vt == VT_I1) {
            dst_buf[8] = val;
            ok = true;
        }
        break;
    }
    case VT_I1: {
        const i8 val = static_cast<i8>(src_buf[8]);
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_I2) {
            write_dst_i16(static_cast<i16>(val));
            ok = true;
        } else if (target_vt == VT_UI1) {
            dst_buf[8] = static_cast<u8>(val);
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        } else if (target_vt == VT_BSTR) {
            const GuestAddr bstr = alloc_bstr_from_str(std::to_string(val));
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        }
        break;
    }
    case VT_UI2: {
        const u16 val = static_cast<u16>(read_src_i16());
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_I2) {
            write_dst_i16(static_cast<i16>(val));
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        } else if (target_vt == VT_BSTR) {
            const GuestAddr bstr = alloc_bstr_from_str(std::to_string(val));
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        }
        break;
    }
    case VT_EMPTY:
    case VT_NULL:
        // EMPTY/NULL conversion to scalar Automation values.
        // Script engines coerce EMPTY→NULL after reading a substitution
        // value. Rejecting it surfaces as DISP_E_TYPEMISMATCH (0x80020005).
        if (target_vt == VT_EMPTY || target_vt == VT_NULL) {
            ok = true;
        } else if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(0);
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(0.0);
            ok = true;
        } else if (target_vt == VT_BSTR) {
            const GuestAddr bstr = alloc_bstr_from_str("");
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(0);
            ok = true;
        }
        break;
    case VT_DISPATCH:
    case VT_UNKNOWN: {
        // IDispatch inherits IUnknown, so DISPATCH→UNKNOWN is a tag change.
        // In-place (dest == src): only flip the VARTYPE — ownership stays.
        // Out-of-place: copy the pointer and AddRef.
        // UNKNOWN→DISPATCH needs QI for IID_IDispatch.
        // Numeric coerce: null → 0, non-null → 1 (script engines probe
        // "was an object returned?" this way when they lack a default prop).
        GuestAddr punk = 0;
        std::memcpy(&punk, &src_buf[8], oleaut_ptr_size(m.cpu_is_64bit()));
        if (target_vt == VT_I4 || target_vt == VT_UI4 || target_vt == VT_BOOL) {
            if (target_vt == VT_BOOL) {
                write_dst_i16(punk != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            } else {
                write_dst_i32(punk != 0 ? 1 : 0);
            }
            ok = true;
            break;
        }
        if (target_vt == VT_BSTR) {
            // Fetch the object's default property (DISPID_VALUE) via
            // IDispatch::Invoke(PROPERTYGET). Null → empty BSTR.
            if (punk == 0) {
                const GuestAddr bstr = alloc_bstr_from_str("");
                write_dst_ptr(bstr);
                ok = (bstr != 0);
                break;
            }
            if (!m_guest_call) break;

            GuestAddr disp = static_cast<GuestAddr>(punk);
            if (src_vt == VT_UNKNOWN) {
                constexpr std::array<u8, 16> iid_dispatch{
                    0x00, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
                const GuestAddr iid = scratch_alloc(16);
                const GuestAddr out = scratch_alloc(8);
                if (iid == 0 || out == 0) break;
                (void)m.memory().write_bytes(iid, iid_dispatch.data(), 16);
                (void)m.write_ptr(out, 0);
                auto vtable_r = m.read_ptr(static_cast<GuestAddr>(punk));
                if (!vtable_r.is_ok() || vtable_r.value() == 0) break;
                auto qi_r =
                    m.read_ptr(static_cast<GuestAddr>(vtable_r.value()));
                if (!qi_r.is_ok() || qi_r.value() == 0) break;
                const u64 a[3] = {static_cast<u64>(punk),
                                  static_cast<u64>(iid),
                                  static_cast<u64>(out)};
                if (static_cast<u32>(m_guest_call(
                        static_cast<GuestAddr>(qi_r.value()), a, 3, m.cpu_is_64bit())) !=
                    S_OK) {
                    break;
                }
                disp = static_cast<GuestAddr>(m.read_ptr(out).value_or(0));
                if (disp == 0) break;
            }

            // IDispatch::Invoke(DISPID_VALUE=0, IID_NULL, lcid=0,
            //                   DISPATCH_PROPERTYGET=2, empty DISPPARAMS,
            //                   &result, NULL, NULL)
            const GuestAddr iid_null = scratch_alloc(16);
            const GuestAddr params   = scratch_alloc(16);
            const GuestAddr result   = scratch_alloc(24);
            if (iid_null == 0 || params == 0 || result == 0) break;
            u8 zeros16[16]{};
            (void)m.memory().write_bytes(iid_null, zeros16, 16);
            (void)m.memory().write_bytes(params, zeros16, 16);
            (void)m.memory().write_bytes(result, zeros16, 16);
            (void)m.memory().write_bytes(result + 16, zeros16, 8);

            auto vt_r = m.read_ptr(disp);
            if (!vt_r.is_ok() || vt_r.value() == 0) break;
            // IDispatch vtable slot 6 = Invoke.
            auto invoke_r = m.read_ptr(static_cast<GuestAddr>(vt_r.value()) +
                                       6 * oleaut_ptr_size(m.cpu_is_64bit()));
            if (!invoke_r.is_ok() || invoke_r.value() == 0) break;

            const u64 args[9] = {
                static_cast<u64>(disp),
                0, // DISPID_VALUE
                static_cast<u64>(iid_null),
                0, // lcid
                2, // DISPATCH_PROPERTYGET
                static_cast<u64>(params),
                static_cast<u64>(result),
                0, // pExcepInfo
                0, // puArgErr
            };
            const u32 hr = static_cast<u32>(m_guest_call(
                static_cast<GuestAddr>(invoke_r.value()), args, 9, m.cpu_is_64bit()));
            if (hr != S_OK) {
                VITREO_TRACE("[oleaut32]   default-prop Invoke hr=0x%08x\n", hr);
                break;
            }
            const u16 got_vt =
                static_cast<u16>(m.read_u16(result).value_or(0));
            if (got_vt == VT_BSTR) {
                write_dst_ptr(static_cast<GuestAddr>(
                    m.read_ptr(result + 8).value_or(0)));
                ok = true;
            } else {
                // Recurse to coerce the default-prop result to BSTR.
                return variant_change_type_ex(m, pvarg_dest, result, 0,
                                              VT_BSTR);
            }
            break;
        }
        if (src_vt == VT_DISPATCH && target_vt == VT_UNKNOWN) {
            write_dst_ptr(punk);
            ok = true;
            if (punk != 0 && pvarg_dest != pvar_src && m_guest_call) {
                auto vtable_r = m.read_ptr(static_cast<GuestAddr>(punk));
                if (vtable_r.is_ok() && vtable_r.value() != 0) {
                    auto addref_r = m.read_ptr(
                        static_cast<GuestAddr>(vtable_r.value()) +
                        oleaut_ptr_size(m.cpu_is_64bit()));
                    if (addref_r.is_ok() && addref_r.value() != 0) {
                        const u64 a[1] = {static_cast<u64>(punk)};
                        (void)m_guest_call(
                            static_cast<GuestAddr>(addref_r.value()), a, 1,
                            m.cpu_is_64bit());
                    }
                }
            }
        } else if (src_vt == VT_UNKNOWN && target_vt == VT_DISPATCH) {
            if (punk == 0) {
                write_dst_ptr(0);
                ok = true;
            } else if (m_guest_call) {
                // QI for IID_IDispatch ({00020400-0000-0000-C000-000000000046}).
                constexpr std::array<u8, 16> iid_dispatch{
                    0x00, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
                const GuestAddr iid = scratch_alloc(16);
                const GuestAddr out = scratch_alloc(8);
                if (iid != 0 && out != 0 &&
                    m.memory()
                        .write_bytes(iid, iid_dispatch.data(), 16)
                        .is_ok()) {
                    (void)m.write_ptr(out, 0);
                    auto vtable_r = m.read_ptr(static_cast<GuestAddr>(punk));
                    if (vtable_r.is_ok() && vtable_r.value() != 0) {
                        auto qi_r = m.read_ptr(
                            static_cast<GuestAddr>(vtable_r.value()));
                        if (qi_r.is_ok() && qi_r.value() != 0) {
                            const u64 a[3] = {static_cast<u64>(punk),
                                              static_cast<u64>(iid),
                                              static_cast<u64>(out)};
                            const u32 hr = static_cast<u32>(m_guest_call(
                                static_cast<GuestAddr>(qi_r.value()), a, 3,
                                m.cpu_is_64bit()));
                            if (hr == S_OK) {
                                write_dst_ptr(static_cast<GuestAddr>(
                                    m.read_ptr(out).value_or(0)));
                                ok = true;
                            }
                        }
                    }
                }
            }
        }
        break;
    }
    default:
        break;
    }

    if (!ok) {
        VITREO_TRACE("[oleaut32]   -> DISP_E_TYPEMISMATCH (srcVT=%u -> targetVT=%u)\n",
                     src_vt, target_vt);
        return DISP_E_TYPEMISMATCH;
    }

    (void)m.memory().write_bytes(pvarg_dest, dst_buf, vsz);
    VITREO_TRACE("[oleaut32]   -> S_OK\n");
    return S_OK;
}

u32 OleAut32Manager::disp_get_param(Marshaler& m) {
    // HRESULT DispGetParam(DISPPARAMS*, UINT position, VARTYPE vtTarg,
    //                      VARIANT* pvarResult, UINT* puArgErr)
    constexpr u32 DISP_E_PARAMNOTFOUND = 0x80020004u;
    constexpr u32 E_INVALIDARG_HR      = 0x80070057u;

    const GuestAddr pdispparams = m.arg_ptr(0);
    const u32       position    = static_cast<u32>(m.arg(1));
    const u16       vt_targ     = static_cast<u16>(m.arg(2));
    const GuestAddr pvar_result = m.arg_ptr(3);
    const GuestAddr pu_arg_err  = m.arg_ptr(4);

    auto fail = [&](u32 hr, u32 arg_index) -> u32 {
        if (pu_arg_err != 0) (void)m.write_u32(pu_arg_err, arg_index);
        VITREO_TRACE("[oleaut32] DispGetParam pos=%u vt=%u -> 0x%08x\n",
                     position, vt_targ, hr);
        return hr;
    };

    if (pdispparams == 0 || pvar_result == 0) {
        return fail(E_INVALIDARG_HR, 0);
    }

    // DISPPARAMS: rgvarg*, rgdispidNamedArgs*, cArgs, cNamedArgs.
    // Pointers are 4 bytes on x86 and 8 on x64; UINT fields stay 4 bytes.
    const u32 ptr_size = oleaut_ptr_size(m.cpu_is_64bit());
    const u32 variant_size = oleaut_variant_size(m.cpu_is_64bit());
    const GuestAddr rgvarg =
        static_cast<GuestAddr>(m.read_ptr(pdispparams).value_or(0));
    const GuestAddr rgdispid = static_cast<GuestAddr>(
        m.read_ptr(pdispparams + ptr_size).value_or(0));
    const u32 c_args =
        static_cast<u32>(m.read_u32(pdispparams + 2u * ptr_size).value_or(0));
    const u32 c_named =
        static_cast<u32>(m.read_u32(pdispparams + 2u * ptr_size + 4).value_or(0));

    u32 pos = 0;
    if (position < c_args) {
        pos = c_args - position - 1;
    } else {
        // Named-arg lookup: DISPID == position.
        bool found = false;
        for (u32 i = 0; i < c_named; ++i) {
            if (rgdispid == 0) break;
            const i32 id =
                static_cast<i32>(m.read_u32(rgdispid + i * 4).value_or(0));
            if (static_cast<u32>(id) == position) {
                pos = i;
                found = true;
                break;
            }
        }
        if (!found) return fail(DISP_E_PARAMNOTFOUND, position);
    }

    if (c_args > 0 && rgvarg == 0) return fail(E_INVALIDARG_HR, pos);

    const GuestAddr src = rgvarg + static_cast<GuestAddr>(pos) * variant_size;

    // Coercion re-enters variant_change_type_ex(dest, src, flags=0, vtTarg) directly with
    // explicit guest addresses. Known gap: its VT_BOOL->VT_BSTR path reads wFlags from
    // m.arg(2) instead of its (unnamed) wFlags parameter, so under DispGetParam it sees
    // vtTarg there. Fixing that changes behaviour, so it is left as a separate follow-up.

    // Clear destination first (VariantInit).
    {
        u8 zeros[24]{};
        (void)m.memory().write_bytes(pvar_result, zeros, variant_size);
    }

    const u16 src_vt =
        static_cast<u16>(m.read_u16(src).value_or(0));
    if (src_vt == vt_targ) {
        u8 buf[24]{};
        (void)m.memory().read_bytes(src, buf, variant_size);
        (void)m.memory().write_bytes(pvar_result, buf, variant_size);
        VITREO_TRACE("[oleaut32] DispGetParam pos=%u vt=%u (same) -> S_OK\n",
                     position, vt_targ);
        return S_OK;
    }

    u32 hr = variant_change_type_ex(m, pvar_result, src, 0, vt_targ);
    if (hr != S_OK) {
        if (pu_arg_err != 0) (void)m.write_u32(pu_arg_err, pos);
        VITREO_TRACE("[oleaut32] DispGetParam pos=%u srcVT=%u -> vt=%u "
                     "hr=0x%08x\n",
                     position, src_vt, vt_targ, hr);
        return hr;
    }
    VITREO_TRACE("[oleaut32] DispGetParam pos=%u srcVT=%u -> vt=%u S_OK\n",
                 position, src_vt, vt_targ);
    return S_OK;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void oleaut32::impl_VariantChangeType(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.variant_change_type(m);
    m.set_return(rc);
}

} // namespace vitreo::winapi
