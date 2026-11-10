// SPDX-License-Identifier: LGPL-2.1-only

/// @file oleaut32_dispatch.cpp
/// @brief The Automation call engine: DispCallFunc, ITypeInfo::Invoke,
///        DispInvoke, and the per-thread error object (Set/GetErrorInfo).
///
/// Sources: Microsoft Learn ITypeInfo::Invoke, IDispatch::Invoke, DispInvoke,
/// DispCallFunc, EXCEPINFO, SetErrorInfo, GetErrorInfo; SDK oaidl.h, oleauto.h,
/// winerror.h, wtypes.h (constant values, audited by
/// tests/oracle_data/oleaut32_dispatch_constants.expected). Everything Learn
/// does not state was measured on Windows with
/// tools/oracle_probe/typeinfo_invoke_probe.c; the recorded behaviour is in
/// tests/oracle_data/typeinfo_invoke_probe.{x86,x64}.expected and summarised
/// in .docs/contracts/oleaut32.md.

#include "oleaut32.hpp"
#include "oleaut32_dispatch_constants.hpp"
#include "winapi/com/oleaut32/oleaut32_internal.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::winapi {

using namespace oleaut32_dispatch;

namespace {

constexpr u32 E_INVALIDARG_HR  = 0x80070057u;
constexpr u32 E_OUTOFMEMORY_HR = 0x8007000Eu;
constexpr u32 E_NOTIMPL_HR     = 0x80004001u;
constexpr u32 E_UNEXPECTED_HR  = 0x8000FFFFu;
constexpr u32 S_FALSE_HR       = 0x00000001u;

/// ITypeInfo vtable slot of Invoke (IUnknown 3 + GetTypeAttr .. GetIDsOfNames).
constexpr u32 kTypeInfoInvokeSlot = 11;
/// IDispatch vtable slot of Invoke.
constexpr u32 kDispatchInvokeSlot = 6;

[[nodiscard]] bool failed(u32 hr) { return (hr & 0x80000000u) != 0; }

void unimplemented_once(const char* what) {
    static std::mutex mutex;
    static std::vector<std::string> seen;
    std::lock_guard lock(mutex);
    if (std::find(seen.begin(), seen.end(), what) != seen.end()) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s\n", what);
}

/// The calling thread's error object (SetErrorInfo / GetErrorInfo). A guest
/// thread runs on its own host thread, so a host thread_local is per guest
/// thread exactly as the Windows error object is.
thread_local GuestAddr t_error_info = 0;

/// Valid DispCallFunc return types (measured, both architectures):
/// EMPTY..UINT except VOID-and-up markers; VT_ARRAY of them; VT_BYREF only
/// on x64. VOID, HRESULT, PTR, SAFEARRAY, USERDEFINED, LPSTR, LPWSTR,
/// RECORD, INT_PTR, UINT_PTR, CLSID and undefined values are E_INVALIDARG.
[[nodiscard]] bool return_base_valid(u16 vt) {
    switch (vt) {
    case VT_EMPTY: case VT_NULL: case VT_I2: case VT_I4: case VT_R4:
    case VT_R8: case VT_CY: case VT_DATE: case VT_BSTR: case VT_DISPATCH:
    case VT_ERROR: case VT_BOOL: case VT_VARIANT: case VT_UNKNOWN:
    case VT_DECIMAL: case VT_I1: case VT_UI1: case VT_UI2: case VT_UI4:
    case VT_I8: case VT_UI8: case VT_INT: case VT_UINT:
        return true;
    default:
        return false;
    }
}

/// x86 stack bytes DispCallFunc pushes for an argument of this VARTYPE, or
/// -1 when it refuses the type (E_INVALIDARG). Measured: `dcf arg.*` rows.
[[nodiscard]] int x86_arg_bytes(u16 vt) {
    if ((vt & VT_VECTOR) != 0) return -1;
    if ((vt & (VT_ARRAY | VT_BYREF)) != 0) return 4;
    switch (vt) {
    case VT_EMPTY: return 0;
    case VT_NULL: case VT_I2: case VT_I4: case VT_R4: case VT_BSTR:
    case VT_DISPATCH: case VT_ERROR: case VT_BOOL: case VT_UNKNOWN:
    case VT_I1: case VT_UI1: case VT_UI2: case VT_UI4: case VT_INT:
    case VT_UINT: case VT_RECORD:
        return 4;
    case VT_R8: case VT_CY: case VT_DATE: case VT_I8: case VT_UI8:
        return 8;
    case VT_VARIANT: case VT_DECIMAL:
        return 16;
    default:
        return -1;
    }
}

/// How x64 DispCallFunc passes an argument (measured): the VARIANT payload
/// for scalar types, a pointer to a copy of the whole VARIANT for VT_VARIANT
/// and every type it does not know, a pointer to a 16-byte copy for
/// VT_DECIMAL. x64 never refuses an argument type.
enum class X64Pass { Payload, VariantCopy, DecimalCopy };
[[nodiscard]] X64Pass x64_pass(u16 vt) {
    if ((vt & (VT_ARRAY | VT_BYREF | VT_VECTOR)) != 0) return X64Pass::Payload;
    if (vt == VT_VARIANT) return X64Pass::VariantCopy;
    if (vt == VT_DECIMAL) return X64Pass::DecimalCopy;
    return vt <= VT_INT_PTR ? X64Pass::Payload : X64Pass::VariantCopy;
}

/// Types returned through a hidden pointer (struct return): measured
/// `RetCYh`, `RetVar`, `RetDec` on both architectures.
[[nodiscard]] bool hidden_return(u16 vt) {
    return vt == VT_CY || vt == VT_VARIANT || vt == VT_DECIMAL;
}

[[nodiscard]] bool fp_return(u16 vt) {
    return vt == VT_R4 || vt == VT_R8 || vt == VT_DATE;
}

/// Payload width a VARIANT stores for a scalar VARTYPE.
[[nodiscard]] u32 payload_width(u16 vt, u32 ptr_size) {
    if ((vt & (VT_ARRAY | VT_BYREF)) != 0) return ptr_size;
    switch (vt) {
    case VT_I1: case VT_UI1: return 1;
    case VT_I2: case VT_UI2: case VT_BOOL: return 2;
    case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4:
    case VT_ERROR: return 4;
    case VT_R8: case VT_CY: case VT_DATE: case VT_I8: case VT_UI8: return 8;
    case VT_BSTR: case VT_DISPATCH: case VT_UNKNOWN: return ptr_size;
    default: return 0;
    }
}

/// A guest allocation released when the scope ends.
class GuestBlock {
public:
    GuestBlock(OleAut32Manager& owner, Marshaler& m, u64 size)
        : m_owner(owner), m_address(owner.oa_alloc(m, size)) {}
    ~GuestBlock() { if (m_address != 0) m_owner.oa_free(m_address); }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    [[nodiscard]] GuestAddr get() const { return m_address; }
private:
    OleAut32Manager& m_owner;
    GuestAddr m_address;
};

} // namespace

// =============================================================================
// Thread error object
// =============================================================================

u32 OleAut32Manager::set_error_info(Marshaler& m) {
    // HRESULT SetErrorInfo(ULONG dwReserved, IErrorInfo* perrinfo)
    const u32 reserved = static_cast<u32>(m.arg(0));
    const GuestAddr info = m.arg_ptr(1);
    if (reserved != 0) return E_INVALIDARG_HR;
    // Learn: the error object replaces the thread's current one; NULL clears.
    com_add_ref(m, info);
    const GuestAddr old = t_error_info;
    t_error_info = info;
    com_release(m, old);
    return S_OK;
}

u32 OleAut32Manager::get_error_info(Marshaler& m) {
    // HRESULT GetErrorInfo(ULONG dwReserved, IErrorInfo** pperrinfo)
    const u32 reserved = static_cast<u32>(m.arg(0));
    const GuestAddr out = m.arg_ptr(1);
    if (reserved != 0 || out == 0) return E_INVALIDARG_HR;
    // Learn: the caller receives the thread's reference and the thread's
    // error object is cleared; S_FALSE when there is none.
    const GuestAddr info = t_error_info;
    t_error_info = 0;
    if (m.write_ptr(out, info).is_err()) {
        t_error_info = info;
        return E_INVALIDARG_HR;
    }
    return info != 0 ? S_OK : S_FALSE_HR;
}

void OleAut32Manager::fill_excepinfo(Marshaler& m, GuestAddr excepinfo, u32 hr) {
    // Measured (`Fail.*`, `Item.get.unset*` rows): every field is zeroed,
    // scode is the member's HRESULT, and when the thread has an error object
    // its source / description / help file / help context are copied in --
    // whether or not the object implements ISupportErrorInfo (no
    // QueryInterface is made). The error object is consumed. With a NULL
    // pExcepInfo nothing is written and the error object stays set.
    if (excepinfo == 0) return;
    const bool is64 = m.cpu_is_64bit();
    const ExcepInfoLayout& layout = is64 ? kExcepInfoX64 : kExcepInfoX86;
    std::vector<Byte> zeros(layout.size, 0);
    if (m.memory().write_bytes(excepinfo, zeros.data(), zeros.size()).is_err()) return;
    (void)m.write_u32(excepinfo + layout.scode, hr);

    const GuestAddr info = t_error_info;
    t_error_info = 0;
    if (info == 0 || !m_guest_call) return;
    const u32 ps = oleaut_ptr_size(is64);
    const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(info).value_or(0));
    GuestBlock out(*this, m, 8);
    auto method = [&](u32 slot) -> GuestAddr {
        return vtbl == 0 ? 0 : static_cast<GuestAddr>(
            m.read_ptr(vtbl + slot * ps).value_or(0));
    };
    auto fetch_ptr = [&](u32 slot, u32 offset) {
        const GuestAddr fn = method(slot);
        if (fn == 0 || out.get() == 0) return;
        (void)m.write_u64(out.get(), 0);
        const u64 args[2] = {info, out.get()};
        if (!failed(static_cast<u32>(m_guest_call(fn, args, 2, is64)))) {
            (void)m.write_ptr(excepinfo + offset, m.read_ptr(out.get()).value_or(0));
        }
    };
    // IErrorInfo: GetGUID 3, GetSource 4, GetDescription 5, GetHelpFile 6,
    // GetHelpContext 7 (oaidl.h).
    fetch_ptr(4, layout.bstrSource);
    fetch_ptr(5, layout.bstrDescription);
    fetch_ptr(6, layout.bstrHelpFile);
    if (const GuestAddr fn = method(7); fn != 0 && out.get() != 0) {
        (void)m.write_u64(out.get(), 0);
        const u64 args[2] = {info, out.get()};
        if (!failed(static_cast<u32>(m_guest_call(fn, args, 2, is64)))) {
            (void)m.write_u32(excepinfo + layout.dwHelpContext,
                              m.read_u32(out.get()).value_or(0));
        }
    }
    com_release(m, info);
}

GuestAddr OleAut32Manager::com_query_interface(Marshaler& m, GuestAddr punk,
                                               const std::array<u8, 16>& iid) {
    if (punk == 0 || !m_guest_call) return 0;
    const bool is64 = m.cpu_is_64bit();
    GuestBlock block(*this, m, 24);
    if (block.get() == 0) return 0;
    (void)m.memory().write_bytes(block.get(), iid.data(), iid.size());
    (void)m.write_u64(block.get() + 16, 0);
    const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(punk).value_or(0));
    const GuestAddr qi = vtbl == 0 ? 0 : static_cast<GuestAddr>(m.read_ptr(vtbl).value_or(0));
    if (qi == 0) return 0;
    const u64 args[3] = {punk, block.get(), block.get() + 16};
    if (failed(static_cast<u32>(m_guest_call(qi, args, 3, is64)))) return 0;
    return static_cast<GuestAddr>(m.read_ptr(block.get() + 16).value_or(0));
}

// =============================================================================
// DispCallFunc
// =============================================================================

u32 OleAut32Manager::call_function(Marshaler& m, GuestAddr instance, u64 ovft,
                                   u32 cc, u16 vt_return,
                                   std::span<const u16> vts,
                                   std::span<const GuestAddr> args,
                                   GuestAddr result) {
    const bool is64 = m.cpu_is_64bit();
    const u32 ps = oleaut_ptr_size(is64);
    const u32 vsz = oleaut_variant_size(is64);

    // Calling convention: x86 accepts CC_CDECL and CC_STDCALL only; x64 has
    // one convention and ignores cc (measured `dcf cc.*`).
    if (!is64 && cc != CC_CDECL && cc != CC_STDCALL) return E_INVALIDARG_HR;

    // Return type (measured `dcf ret.*`).
    {
        const u16 base = static_cast<u16>(vt_return & ~(VT_ARRAY | VT_BYREF));
        const u16 flags = static_cast<u16>(vt_return & 0xF000);
        bool valid = return_base_valid(base);
        if (flags & ~(VT_ARRAY | VT_BYREF)) valid = false;
        if (!is64 && (vt_return & VT_BYREF)) valid = false;
        if (!valid) return E_INVALIDARG_HR;
    }
    if (result == 0) {
        unimplemented_once("oleaut32!DispCallFunc: NULL pvargResult (unmeasured)");
        return E_INVALIDARG_HR;
    }
    if (!m_guest_call_ex) {
        unimplemented_once("oleaut32!DispCallFunc: no guest-call bridge wired");
        return E_UNEXPECTED_HR;
    }

    // Target: a vtable slot of pvInstance, or oVft itself when there is no
    // instance (measured `free_cdecl` / `free_stdcall`).
    GuestAddr target = static_cast<GuestAddr>(ovft);
    if (instance != 0) {
        auto vtbl = m.read_ptr(instance);
        if (!vtbl.is_ok() || vtbl.value() == 0) return E_INVALIDARG_HR;
        auto fn = m.read_ptr(static_cast<GuestAddr>(vtbl.value() + ovft));
        if (!fn.is_ok()) return E_INVALIDARG_HR;
        target = static_cast<GuestAddr>(fn.value());
    }
    if (target == 0) return E_INVALIDARG_HR;

    // x86 classification first so a refused type fails before any call.
    if (!is64) {
        for (u16 vt : vts) {
            if (x86_arg_bytes(vt) < 0) return E_INVALIDARG_HR;
        }
    }

    std::vector<u64> words;
    words.reserve(vts.size() * 4 + 2);
    if (instance != 0) words.push_back(instance);
    const bool hidden = hidden_return(vt_return);
    if (hidden) {
        // VARIANT and DECIMAL are written over the result itself; a CY
        // return lands in the result's payload.
        words.push_back(vt_return == VT_CY ? result + 8 : result);
    }

    // x64 copies for VARIANT / DECIMAL / unknown-type arguments.
    GuestBlock copies(*this, m, is64 ? std::max<u64>(1, vts.size()) * vsz : 8);
    if (is64 && copies.get() == 0) return E_OUTOFMEMORY_HR;

    for (usize i = 0; i < vts.size(); ++i) {
        const u16 vt = vts[i];
        const GuestAddr var = args[i];
        if (var == 0) return E_INVALIDARG_HR;
        if (!is64) {
            const int bytes = x86_arg_bytes(vt);
            if (vt == VT_BOOL) {
                words.push_back(m.read_u16(var + 8).value_or(0));
            } else if (bytes == 16) {
                for (u32 k = 0; k < 4; ++k) words.push_back(m.read_u32(var + 4 * k).value_or(0));
            } else {
                for (int k = 0; k < bytes / 4; ++k)
                    words.push_back(m.read_u32(var + 8 + 4 * k).value_or(0));
            }
            continue;
        }
        switch (x64_pass(vt)) {
        case X64Pass::Payload:
            words.push_back(m.read_u64(var + 8).value_or(0));
            break;
        case X64Pass::VariantCopy:
        case X64Pass::DecimalCopy: {
            const u32 size = x64_pass(vt) == X64Pass::DecimalCopy ? 16u : vsz;
            std::vector<Byte> bytes(size, 0);
            (void)m.memory().read_bytes(var, bytes.data(), size);
            const GuestAddr copy = copies.get() + i * vsz;
            (void)m.memory().write_bytes(copy, bytes.data(), size);
            words.push_back(copy);
            break;
        }
        }
    }

    const bool pop_st0 = !is64 && fp_return(vt_return);
    const GuestCallResult call =
        m_guest_call_ex(target, words.data(), words.size(), is64, pop_st0);
    if (!call.completed) return E_UNEXPECTED_HR;

    // x86 stdcall: the callee must remove exactly what was pushed, or the
    // call is DISP_E_BADCALLEE (measured `imbalance`, mismatched-size
    // `dcf arg` / hidden-return rows). cdecl callers restore ESP themselves.
    if (!is64 && cc == CC_STDCALL && call.callee_popped != 4u * words.size()) {
        return DISP_E_BADCALLEE;
    }

    if (hidden) {
        if (vt_return != VT_VARIANT) (void)m.write_u16(result, vt_return);
        return S_OK;
    }
    (void)m.write_u16(result, vt_return);
    if (vt_return == VT_EMPTY || vt_return == VT_NULL) return S_OK;
    if (fp_return(vt_return)) {
        if (is64) {
            if (vt_return == VT_R4) (void)m.write_u32(result + 8, static_cast<u32>(call.xmm0));
            else (void)m.write_u64(result + 8, call.xmm0);
        } else if (vt_return == VT_R4) {
            const float value = static_cast<float>(call.st0);
            u32 bits = 0;
            std::memcpy(&bits, &value, 4);
            (void)m.write_u32(result + 8, bits);
        } else {
            u64 bits = 0;
            std::memcpy(&bits, &call.st0, 8);
            (void)m.write_u64(result + 8, bits);
        }
        return S_OK;
    }
    if (is64) {
        (void)m.write_u64(result + 8, call.rax);
    } else if (payload_width(vt_return, ps) == 8) {
        (void)m.write_u32(result + 8, static_cast<u32>(call.rax));
        (void)m.write_u32(result + 12, static_cast<u32>(call.rdx));
    } else {
        (void)m.write_u32(result + 8, static_cast<u32>(call.rax));
    }
    return S_OK;
}

u32 OleAut32Manager::disp_call_func(Marshaler& m) {
    // HRESULT DispCallFunc(void* pvInstance, ULONG_PTR oVft, CALLCONV cc,
    //   VARTYPE vtReturn, UINT cActuals, VARTYPE* prgvt,
    //   VARIANTARG** prgpvarg, VARIANT* pvargResult)
    const GuestAddr instance = m.arg_ptr(0);
    const u64 ovft = m.cpu_is_64bit() ? m.arg(1) : static_cast<u32>(m.arg(1));
    const u32 cc = static_cast<u32>(m.arg(2));
    const u16 vt_return = static_cast<u16>(m.arg(3));
    const u32 count = static_cast<u32>(m.arg(4));
    const GuestAddr prgvt = m.arg_ptr(5);
    const GuestAddr prgpvarg = m.arg_ptr(6);
    const GuestAddr result = m.arg_ptr(7);
    if (count != 0 && (prgvt == 0 || prgpvarg == 0)) return E_INVALIDARG_HR;
    if (count > 0x10000) return E_INVALIDARG_HR;
    const u32 ps = oleaut_ptr_size(m.cpu_is_64bit());
    std::vector<u16> vts(count);
    std::vector<GuestAddr> args(count);
    for (u32 i = 0; i < count; ++i) {
        vts[i] = m.read_u16(prgvt + 2 * i).value_or(0);
        args[i] = static_cast<GuestAddr>(m.read_ptr(prgpvarg + ps * i).value_or(0));
    }
    const u32 hr = call_function(m, instance, ovft, cc, vt_return, vts, args, result);
    VITREO_TRACE("[oleaut32] DispCallFunc inst=0x%llx oVft=0x%llx cc=%u vtRet=%u n=%u -> 0x%08x\n",
                 static_cast<unsigned long long>(instance),
                 static_cast<unsigned long long>(ovft), cc, vt_return, count, hr);
    return hr;
}

// =============================================================================
// ITypeInfo::Invoke
// =============================================================================

namespace {

struct ParamPlan {
    u16 call_vt{0};          ///< VARTYPE handed to the call engine.
    GuestAddr var{0};        ///< VARIANT the engine reads.
    GuestAddr owned{0};      ///< Temporary to VariantClear afterwards.
    GuestAddr slot{0};       ///< Interface [in,out]/[out]: the caller's pointer slot.
    u16 slot_vt{0};          ///< VT_DISPATCH or VT_UNKNOWN: what that slot holds.
    GuestAddr original{0};   ///< The caller's pointer before the call (released after).
};

constexpr std::array<u8, 16> kIidIUnknown{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr std::array<u8, 16> kIidIDispatch{
    0x00, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

/// Trace text for one VARIANT: its VT and, for BSTR (direct or by
/// reference), up to 96 characters of the string; "NULL" for a null BSTR.
std::string describe_variant(Marshaler& m, GuestAddr var) {
    const u16 vt = m.read_u16(var).value_or(0);
    char head[24];
    std::snprintf(head, sizeof head, "vt=0x%x", vt);
    std::string out = head;
    if ((vt & VT_TYPEMASK) != VT_BSTR || (vt & VT_ARRAY) != 0) return out;
    GuestAddr bstr = static_cast<GuestAddr>(m.read_ptr(var + 8).value_or(0));
    if ((vt & VT_BYREF) != 0 && bstr != 0) {
        bstr = static_cast<GuestAddr>(m.read_ptr(bstr).value_or(0));
    }
    if (bstr == 0) return out + " NULL";
    const u32 bytes = m.read_u32(bstr - 4).value_or(0);
    out += " \"";
    for (u32 i = 0; i + 1 < bytes && i < 192; i += 2) {
        const u16 ch = m.read_u16(bstr + i).value_or(0);
        out.push_back(ch >= 0x20 && ch < 0x7F ? static_cast<char>(ch) : '?');
    }
    return out + "\"";
}

} // namespace

u32 OleAut32Manager::invoke_member(Marshaler& m, const LoadedTypeInfo& state,
                                   GuestAddr instance, i32 memid, u32 flags,
                                   GuestAddr params, GuestAddr result,
                                   GuestAddr excepinfo, GuestAddr argerr) {
    const bool is64 = m.cpu_is_64bit();
    const u32 ps = oleaut_ptr_size(is64);
    const u32 vsz = oleaut_variant_size(is64);
    if (state.parent == nullptr) return E_INVALIDARG_HR;
    const LoadedTypeLib& lib = *state.parent;

    // Argument validation (measured `edge`, `M0.flags0`, `Opt.named.*`).
    if (instance == 0 || params == 0) return E_INVALIDARG_HR;
    if ((flags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET | DISPATCH_PROPERTYPUT |
                  DISPATCH_PROPERTYPUTREF)) == 0) {
        return E_INVALIDARG_HR;
    }
    const GuestAddr rgvarg = static_cast<GuestAddr>(m.read_ptr(params).value_or(0));
    const GuestAddr named = static_cast<GuestAddr>(m.read_ptr(params + ps).value_or(0));
    const u32 c_args = m.read_u32(params + 2 * ps).value_or(0);
    const u32 c_named = m.read_u32(params + 2 * ps + 4).value_or(0);
    if (c_named > c_args || (c_named != 0 && named == 0) || (c_args != 0 && rgvarg == 0)) {
        return E_INVALIDARG_HR;
    }

    // Member lookup: this type, then its base interfaces (Learn: "recurses
    // on the base type description"). The first function with the memid
    // whose INVOKEKIND is among wFlags wins, in type-information order.
    const FuncSummary* fn = nullptr;
    const TypeInfoSummary* owner = nullptr;
    {
        std::vector<bool> visited(lib.typeinfos.size(), false);
        u32 index = state.index;
        while (fn == nullptr && index < lib.typeinfos.size() && !visited[index]) {
            visited[index] = true;
            const TypeInfoSummary& info = lib.typeinfos[index];
            for (const FuncSummary& f : info.funcs) {
                if (f.memid == memid && (f.invkind & flags) != 0) {
                    fn = &f;
                    owner = &info;
                    break;
                }
            }
            if (fn != nullptr || info.impl_hrefs.empty()) break;
            const u32 href = info.impl_hrefs[0];
            u32 next = static_cast<u32>(lib.typeinfos.size());
            for (u32 k = 0; k < lib.typeinfos.size(); ++k) {
                if (lib.typeinfos[k].reference == href) { next = k; break; }
            }
            index = next;   // imported bases (IDispatch) end the walk
        }
    }
    if (fn == nullptr) return DISP_E_MEMBERNOTFOUND;

    if (fn->funckind != FUNC_PUREVIRTUAL && fn->funckind != FUNC_VIRTUAL) {
        unimplemented_once("oleaut32!ITypeInfo::Invoke: FUNC_NONVIRTUAL / FUNC_STATIC / "
                           "FUNC_DISPATCH member");
        return E_NOTIMPL_HR;
    }
    if (fn->param_descriptions.size() != fn->c_params ||
        fn->param_shapes.size() != fn->c_params ||
        fn->param_flags.size() != fn->c_params || fn->result_description.tags.empty()) {
        return E_NOTIMPL_HR;
    }

    // Visible (dispatch) parameters exclude [lcid] and [retval].
    std::vector<u32> visible;
    i32 retval_index = -1;
    for (u32 i = 0; i < fn->c_params; ++i) {
        const u16 pf = fn->param_flags[i];
        if (pf & PARAMFLAG_FRETVAL) { retval_index = static_cast<i32>(i); continue; }
        if (pf & PARAMFLAG_FLCID) continue;
        visible.push_back(i);
    }
    const u32 c_visible = static_cast<u32>(visible.size());
    const bool is_put = (fn->invkind & (INVOKE_PROPERTYPUT | INVOKE_PROPERTYPUTREF)) != 0;

    std::vector<i32> named_ids(c_named);
    for (u32 j = 0; j < c_named; ++j) {
        named_ids[j] = static_cast<i32>(m.read_u32(named + 4 * j).value_or(0));
    }

    // Collection access (measured `Prop.get.extra`, `Obj.get.index*`,
    // `M0.extra.withget`, `Prop.put.withget`): a property get of a member
    // that takes no arguments, given arguments, calls the member and applies
    // the arguments to the DISPID_VALUE of the IDispatch it returns.
    const bool collection = (flags & DISPATCH_PROPERTYGET) != 0 && c_visible == 0 && c_args > 0;
    if (collection && retval_index < 0) return DISP_E_NOTACOLLECTION;

    if (!collection) {
        // Property put: the new value must be the DISPID_PROPERTYPUT named
        // argument (Learn IDispatch::Invoke remarks; measured
        // `Prop.put.unnamed`).
        if (is_put && std::find(named_ids.begin(), named_ids.end(), DISPID_PROPERTYPUT) ==
                          named_ids.end()) {
            return DISP_E_PARAMNOTFOUND;
        }
        if (c_args > c_visible) return DISP_E_BADPARAMCOUNT;
    }

    // Map rgvarg onto the visible parameters.
    std::vector<i32> slot(c_visible, -1);
    bool named_error = false;
    if (!collection) {
        const u32 positional = c_args - c_named;
        for (u32 k = 0; k < positional && k < c_visible; ++k) {
            slot[k] = static_cast<i32>(c_args - 1 - k);
        }
        for (u32 j = 0; j < c_named; ++j) {
            i32 target = named_ids[j];
            if (target == DISPID_PROPERTYPUT && is_put) target = static_cast<i32>(c_visible) - 1;
            if (target < 0 || static_cast<u32>(target) >= c_visible || slot[target] != -1) {
                named_error = true;
                continue;
            }
            slot[target] = static_cast<i32>(j);
        }
        // A required parameter left without an argument is a count error
        // before any named-argument error (measured `Named.*`, `Opt.named.*`).
        for (u32 k = 0; k < c_visible; ++k) {
            const u16 pf = fn->param_flags[visible[k]];
            if (slot[k] == -1 && (pf & (PARAMFLAG_FOPT | PARAMFLAG_FHASDEFAULT)) == 0) {
                return DISP_E_BADPARAMCOUNT;
            }
        }
        if (named_error) return DISP_E_PARAMNOTFOUND;
    }

    // Temporaries: one VARIANT per parameter, one for the [retval] value,
    // one for the function's own return value.
    GuestBlock block(*this, m, static_cast<u64>(fn->c_params + 3) * vsz);
    if (block.get() == 0) return E_OUTOFMEMORY_HR;
    const GuestAddr retval_store = block.get() + static_cast<u64>(fn->c_params) * vsz;
    const GuestAddr call_result = retval_store + vsz;

    std::vector<ParamPlan> plan(fn->c_params);
    auto release_plan = [&]() {
        for (const ParamPlan& p : plan) {
            if (p.owned != 0) (void)variant_clear_at(m, p.owned);
        }
    };
    auto fail_arg = [&](u32 hr, i32 index) {
        release_plan();
        if (argerr != 0 && index >= 0) (void)m.write_u32(argerr, static_cast<u32>(index));
        return hr;
    };
    auto set_vt = [&](GuestAddr var, u16 vt) { (void)m.write_u16(var, vt); };

    for (u32 i = 0; i < fn->c_params; ++i) {
        const GuestAddr temp = block.get() + static_cast<u64>(i) * vsz;
        const u16 pf = fn->param_flags[i];
        // Automation shape from the type library: user-defined interface
        // pointers are VT_DISPATCH/VT_UNKNOWN with the declared IID, enums
        // VT_I4, aliases expanded (typelib.cpp shape_of; `ty` rows).
        const ParamShape& shape = fn->param_shapes[i];
        const bool is_ptr = shape.depth == 1;
        const u16 base = shape.vt;
        const bool is_iface = base == VT_DISPATCH || base == VT_UNKNOWN;
        if (!shape.valid || shape.depth > 1 ||
            (payload_width(base, ps) == 0 && base != VT_VARIANT) ||
            (static_cast<i32>(i) == retval_index && !is_ptr)) {
            unimplemented_once("oleaut32!ITypeInfo::Invoke: parameter type (record, union, "
                               "unresolvable imported type, or more than one pointer level)");
            release_plan();
            return E_NOTIMPL_HR;
        }
        // Query `p` for the declared interface; nullptr when it refuses.
        auto query_declared = [&](GuestAddr p) -> GuestAddr {
            return com_query_interface(m, p, shape.iid);
        };

        if (pf & PARAMFLAG_FLCID) {
            // Learn: [lcid] receives the LCID of the type information.
            set_vt(temp, VT_I4);
            (void)m.write_u32(temp + 8, lib.attributes.lcid);
            plan[i] = {VT_I4, temp, 0};
            continue;
        }
        if (static_cast<i32>(i) == retval_index) {
            // [out, retval] T*: storage the member fills; VARIANT* gets the
            // VARIANT itself.
            const GuestAddr payload = base == VT_VARIANT ? retval_store : retval_store + 8;
            set_vt(temp, static_cast<u16>(VT_BYREF | base));
            (void)m.write_ptr(temp + 8, payload);
            plan[i] = {static_cast<u16>(VT_BYREF | base), temp, 0};
            continue;
        }

        const u32 k = static_cast<u32>(std::find(visible.begin(), visible.end(), i) -
                                       visible.begin());
        const i32 s = collection ? -1 : slot[k];
        GuestAddr arg = s >= 0 ? rgvarg + static_cast<u64>(s) * vsz : 0;
        u16 avt = arg != 0 ? m.read_u16(arg).value_or(0) : 0;
        if (!is_ptr && base != VT_VARIANT && avt == (VT_BYREF | VT_VARIANT)) {
            // A by-value parameter reads through VT_BYREF|VT_VARIANT
            // (measured `Ints.byrefvariant`, `InDual.byrefvariant`).
            const GuestAddr inner = static_cast<GuestAddr>(m.read_ptr(arg + 8).value_or(0));
            if (inner == 0) return fail_arg(DISP_E_TYPEMISMATCH, s);
            arg = inner;
            avt = m.read_u16(arg).value_or(0);
        }
        const bool supplied_missing = arg != 0 && avt == VT_ERROR &&
            m.read_u32(arg + 8).value_or(0) == DISP_E_PARAMNOTFOUND;

        if (arg == 0 || (supplied_missing && base != VT_VARIANT)) {
            if (pf & PARAMFLAG_FHASDEFAULT) {
                if (i >= fn->param_defaults.size() || !fn->param_defaults[i]) {
                    unimplemented_once("oleaut32!ITypeInfo::Invoke: default value "
                                       "encoding not decoded");
                    release_plan();
                    return E_NOTIMPL_HR;
                }
                const ParamDefault& def = *fn->param_defaults[i];
                const GuestAddr value = temp;
                set_vt(temp, def.vt);
                if (def.vt == VT_BSTR) {
                    std::vector<Byte> utf16;
                    for (unsigned char c : def.text) { utf16.push_back(c); utf16.push_back(0); }
                    (void)m.write_ptr(temp + 8, allocate_bstr_bytes(m, utf16));
                } else {
                    (void)m.write_u64(temp + 8, def.bits);
                }
                if (is_ptr) {
                    unimplemented_once("oleaut32!ITypeInfo::Invoke: default value for a "
                                       "pointer parameter");
                    (void)variant_clear_at(m, temp);
                    release_plan();
                    return E_NOTIMPL_HR;
                }
                if (base != VT_VARIANT && def.vt != base) {
                    const GuestAddr converted = retval_store;   // scratch, unused yet
                    if (failed(variant_change_type_ex(m, converted, temp, 0, base))) {
                        (void)variant_clear_at(m, temp);
                        release_plan();
                        return DISP_E_TYPEMISMATCH;
                    }
                    (void)variant_clear_at(m, temp);
                    std::vector<Byte> bytes(vsz);
                    (void)m.memory().read_bytes(converted, bytes.data(), vsz);
                    (void)m.memory().write_bytes(temp, bytes.data(), vsz);
                    std::vector<Byte> zeros(vsz, 0);
                    (void)m.memory().write_bytes(converted, zeros.data(), vsz);
                }
                plan[i] = {base == VT_VARIANT ? VT_VARIANT : base, value, temp};
                continue;
            }
            if (supplied_missing) return fail_arg(DISP_E_PARAMNOTOPTIONAL, s);
            if (base == VT_VARIANT) {
                // Omitted [optional] VARIANT: VT_ERROR / DISP_E_PARAMNOTFOUND.
                set_vt(temp, VT_ERROR);
                (void)m.write_u32(temp + 8, DISP_E_PARAMNOTFOUND);
                if (is_ptr) {
                    unimplemented_once("oleaut32!ITypeInfo::Invoke: omitted optional VARIANT* "
                                       "parameter");
                    release_plan();
                    return E_NOTIMPL_HR;
                }
                plan[i] = {VT_VARIANT, temp, 0};
                continue;
            }
            unimplemented_once("oleaut32!ITypeInfo::Invoke: omitted [optional] parameter "
                               "that is neither VARIANT nor [defaultvalue]");
            release_plan();
            return E_NOTIMPL_HR;
        }

        if (!is_ptr) {
            if (base == VT_VARIANT) {
                // [in] VARIANT by value: the argument as given (measured
                // `Var.*`: VT_BYREF forms pass through, the callee's copy is
                // not seen by the caller).
                std::vector<Byte> bytes(vsz);
                (void)m.memory().read_bytes(arg, bytes.data(), vsz);
                (void)m.memory().write_bytes(temp, bytes.data(), vsz);
                plan[i] = {VT_VARIANT, temp, 0};
                continue;
            }
            if (is_iface) {
                // Interface by value (measured `In*` rows): coerce to the
                // VT (VT_UNKNOWN -> VT_DISPATCH asks for IDispatch; its
                // failure is returned), then QueryInterface for the declared
                // IID; refusal is DISP_E_TYPEMISMATCH. The queried reference
                // is released after the call.
                GuestAddr p = 0;
                bool owned = false;
                if (avt == base) {
                    p = static_cast<GuestAddr>(m.read_ptr(arg + 8).value_or(0));
                } else if (avt == static_cast<u16>(VT_BYREF | base)) {
                    const GuestAddr ptr = static_cast<GuestAddr>(m.read_ptr(arg + 8).value_or(0));
                    if (ptr == 0) return fail_arg(DISP_E_TYPEMISMATCH, s);
                    p = static_cast<GuestAddr>(m.read_ptr(ptr).value_or(0));
                } else {
                    const u32 hr = variant_change_type_ex(m, temp, arg, 0, base);
                    if (failed(hr)) return fail_arg(hr, s);
                    p = static_cast<GuestAddr>(m.read_ptr(temp + 8).value_or(0));
                    owned = true;
                }
                if (shape.has_iid && p != 0) {
                    const GuestAddr q = query_declared(p);
                    if (owned) com_release(m, p);
                    if (q == 0) {
                        set_vt(temp, VT_EMPTY);
                        return fail_arg(DISP_E_TYPEMISMATCH, s);
                    }
                    p = q;
                    owned = true;
                }
                set_vt(temp, base);
                (void)m.write_ptr(temp + 8, p);
                plan[i] = {base, temp, owned ? temp : 0};
                continue;
            }
            if (avt == base) {
                plan[i] = {base, arg, 0};
                continue;
            }
            if (avt == static_cast<u16>(VT_BYREF | base)) {
                const GuestAddr ptr = static_cast<GuestAddr>(m.read_ptr(arg + 8).value_or(0));
                if (ptr == 0) return fail_arg(DISP_E_TYPEMISMATCH, s);
                set_vt(temp, base);
                const u32 width = payload_width(base, ps);
                std::vector<Byte> bytes(width);
                (void)m.memory().read_bytes(ptr, bytes.data(), width);
                (void)m.memory().write_bytes(temp + 8, bytes.data(), width);
                plan[i] = {base, temp, 0};
                continue;
            }
            if (base & VT_ARRAY) {
                // SAFEARRAY(T) takes exactly VT_ARRAY|T (measured `Arr.*`, `InSA*`).
                return fail_arg(DISP_E_TYPEMISMATCH, s);
            }
            const u32 hr = variant_change_type_ex(m, temp, arg, 0, base);
            if (failed(hr)) return fail_arg(hr, s);
            plan[i] = {base, temp, temp};
            continue;
        }

        // Pointer parameter T* (measured `OutLong.*`, `VarPtr.*`, `OutBstr*`).
        const u16 byref = static_cast<u16>(VT_BYREF | base);
        if (is_iface) {
            // IFoo** [in,out] / [out] (measured `InOutDual.*`, `OutDual.*`,
            // `InOutUnk.*`): VT_BYREF|VT_DISPATCH and VT_BYREF|VT_UNKNOWN
            // are both accepted; the current pointer is queried for the
            // declared IID (refusal: DISP_E_TYPEMISMATCH) and the member
            // works on that reference. Afterwards the caller's old pointer
            // is released and the member's result stored in its slot,
            // converted to the slot's VT by QueryInterface when it differs.
            const u16 other = static_cast<u16>(VT_BYREF | (base == VT_DISPATCH ? VT_UNKNOWN : VT_DISPATCH));
            if (avt == byref || avt == other) {
                const GuestAddr slot_ptr = static_cast<GuestAddr>(m.read_ptr(arg + 8).value_or(0));
                if (slot_ptr == 0) return fail_arg(DISP_E_TYPEMISMATCH, s);
                const GuestAddr cur = static_cast<GuestAddr>(m.read_ptr(slot_ptr).value_or(0));
                GuestAddr q = 0;
                if (cur != 0) {
                    if (shape.has_iid) q = query_declared(cur);
                    else if (avt != byref) q = com_query_interface(
                        m, cur, base == VT_DISPATCH ? kIidIDispatch : kIidIUnknown);
                    else { com_add_ref(m, cur); q = cur; }
                    if (q == 0) return fail_arg(DISP_E_TYPEMISMATCH, s);
                }
                set_vt(temp, base);
                (void)m.write_ptr(temp + 8, q);
                plan[i] = {byref, 0, temp, slot_ptr, static_cast<u16>(avt & ~VT_BYREF), cur};
                continue;
            }
            if (avt & VT_BYREF) return fail_arg(DISP_E_TYPEMISMATCH, s);
            GuestAddr p = 0;
            if (pf & PARAMFLAG_FIN) {
                if (avt == base) {
                    p = static_cast<GuestAddr>(m.read_ptr(arg + 8).value_or(0));
                    if (p != 0 && !shape.has_iid) com_add_ref(m, p);
                } else {
                    const u32 hr = variant_change_type_ex(m, temp, arg, 0, base);
                    if (failed(hr)) return fail_arg(hr, s);
                    p = static_cast<GuestAddr>(m.read_ptr(temp + 8).value_or(0));
                    if (shape.has_iid && p != 0) {
                        const GuestAddr q = query_declared(p);
                        com_release(m, p);
                        if (q == 0) { set_vt(temp, VT_EMPTY); return fail_arg(DISP_E_TYPEMISMATCH, s); }
                        p = q;
                    }
                    set_vt(temp, base);
                    (void)m.write_ptr(temp + 8, p);
                    plan[i] = {byref, 0, temp};
                    continue;
                }
                if (shape.has_iid && p != 0) {
                    p = query_declared(p);
                    if (p == 0) return fail_arg(DISP_E_TYPEMISMATCH, s);
                }
            }
            set_vt(temp, base);
            (void)m.write_ptr(temp + 8, p);
            plan[i] = {byref, 0, temp};
            continue;
        }
        if (avt == byref) {
            const GuestAddr ptr = static_cast<GuestAddr>(m.read_ptr(arg + 8).value_or(0));
            if ((pf & PARAMFLAG_FOUT) && !(pf & PARAMFLAG_FIN) && ptr != 0) {
                // [out]: the previous value is released before the call.
                if (base == VT_BSTR) {
                    sys_free_string(m, static_cast<GuestAddr>(m.read_ptr(ptr).value_or(0)));
                    (void)m.write_ptr(ptr, 0);
                } else if (base == VT_VARIANT) {
                    (void)variant_clear_at(m, ptr);
                }
            }
            plan[i] = {byref, arg, 0};
            continue;
        }
        if (base == VT_VARIANT) {
            if (avt & VT_BYREF) {
                // Another by-reference VARIANT: the argument VARIANT itself.
                set_vt(temp, byref);
                (void)m.write_ptr(temp + 8, arg);
                plan[i] = {byref, temp, 0};
                continue;
            }
            // By value: a copy the member may change or clear.
            GuestAddr copy = temp;
            if (failed(variant_copy_at(m, copy, arg))) return fail_arg(E_OUTOFMEMORY_HR, s);
            // The engine needs a VT_BYREF|VT_VARIANT VARIANT pointing at the
            // copy; it lives in the next free temporary slot of this plan.
            plan[i] = {byref, 0, copy};
            continue;
        }
        if (avt & VT_BYREF) return fail_arg(DISP_E_TYPEMISMATCH, s);
        if (pf & PARAMFLAG_FIN) {
            const u32 hr = avt == base ? variant_copy_at(m, temp, arg)
                                       : variant_change_type_ex(m, temp, arg, 0, base);
            if (failed(hr)) return fail_arg(hr, s);
        } else {
            set_vt(temp, base);
        }
        plan[i] = {byref, 0, temp};
    }

    // Pointer temporaries: the engine reads a VT_BYREF VARIANT whose payload
    // points at the temporary's payload (or the temporary VARIANT itself).
    GuestBlock refs(*this, m, static_cast<u64>(std::max<u32>(1, fn->c_params)) * vsz);
    if (refs.get() == 0) { release_plan(); return E_OUTOFMEMORY_HR; }
    for (u32 i = 0; i < fn->c_params; ++i) {
        ParamPlan& p = plan[i];
        if (p.var != 0) continue;
        const GuestAddr ref = refs.get() + static_cast<u64>(i) * vsz;
        (void)m.write_u16(ref, p.call_vt);
        const u16 base = static_cast<u16>(p.call_vt & ~VT_BYREF);
        (void)m.write_ptr(ref + 8, base == VT_VARIANT ? p.owned : p.owned + 8);
        p.var = ref;
    }

    std::vector<u16> vts(fn->c_params);
    std::vector<GuestAddr> vars(fn->c_params);
    for (u32 i = 0; i < fn->c_params; ++i) { vts[i] = plan[i].call_vt; vars[i] = plan[i].var; }

    // The function's own return type: HRESULT is an SCODE (VT_ERROR) to the
    // engine; void is VT_EMPTY.
    u16 ret_vt = fn->result_description.tags.back();
    const bool returns_hresult = fn->result_description.tags.size() == 1 && ret_vt == VT_HRESULT;
    if (returns_hresult) ret_vt = VT_ERROR;
    else if (fn->result_description.tags.size() == 1 && ret_vt == VT_VOID) ret_vt = VT_EMPTY;
    else if (fn->result_description.tags.size() != 1 || !return_base_valid(ret_vt)) {
        unimplemented_once("oleaut32!ITypeInfo::Invoke: member return type");
        release_plan();
        return E_NOTIMPL_HR;
    }

    // pVarResult is initialised once the arguments are accepted, property
    // puts included (measured: `res=0` on every row that reached the call).
    if (result != 0 && !collection) {
        std::vector<Byte> zeros(vsz, 0);
        (void)m.memory().write_bytes(result, zeros.data(), vsz);
    }

    // Arguments as the caller passed them (rgvarg order), for the trace.
    std::string traced_args;
#if VITREO_TRACE_ENABLED
    if (::vitreo::detail::g_trace_enabled) {
        for (u32 k = 0; k < c_args; ++k) {
            if (k != 0) traced_args += ", ";
            traced_args += describe_variant(m, rgvarg + static_cast<u64>(k) * vsz);
        }
    }
#endif

    const u32 ovft = reader_vtable_bytes(fn->o_vft, lib.attributes.syskind, ps);
    const u32 call_hr = call_function(m, instance, ovft, fn->callconv, ret_vt, vts, vars,
                                      call_result);
    for (ParamPlan& p : plan) {
        if (p.slot == 0 || failed(call_hr)) continue;
        // Interface [in,out]/[out]: hand the member's reference back.
        GuestAddr out = static_cast<GuestAddr>(m.read_ptr(p.owned + 8).value_or(0));
        const u16 base = static_cast<u16>(p.call_vt & ~VT_BYREF);
        if (out != 0 && p.slot_vt != base) {
            const GuestAddr q = com_query_interface(
                m, out, p.slot_vt == VT_DISPATCH ? kIidIDispatch : kIidIUnknown);
            com_release(m, out);
            out = q;
        }
        com_release(m, p.original);
        (void)m.write_ptr(p.slot, out);
        (void)m.write_u16(p.owned, VT_EMPTY);
    }
    release_plan();
    if (failed(call_hr)) {
        (void)variant_clear_at(m, retval_store);
        return call_hr;
    }

    const u32 member_hr = returns_hresult ? m.read_u32(call_result + 8).value_or(0) : S_OK;
    const char* const owner_name = owner != nullptr ? owner->name.c_str() : "?";
    if (failed(member_hr)) {
        VITREO_TRACE("[oleaut32] ITypeInfo::Invoke %s.%s memid=%d flags=0x%x args=[%s] -> "
                     "member hr=0x%08x\n",
                     owner_name, fn->name.c_str(), memid, flags, traced_args.c_str(), member_hr);
        // Learn: a failing member makes Invoke return DISP_E_EXCEPTION with
        // the details in pExcepInfo.
        (void)variant_clear_at(m, retval_store);
        fill_excepinfo(m, excepinfo, member_hr);
        return DISP_E_EXCEPTION;
    }

    // Package the result.
    if (retval_index >= 0) {
        const u16 base = fn->param_shapes[static_cast<u32>(retval_index)].vt;
        if (base != VT_VARIANT) (void)m.write_u16(retval_store, base);
    } else if (!returns_hresult && ret_vt != VT_EMPTY) {
        std::vector<Byte> bytes(vsz);
        (void)m.memory().read_bytes(call_result, bytes.data(), vsz);
        (void)m.memory().write_bytes(retval_store, bytes.data(), vsz);
    }
    VITREO_TRACE("[oleaut32] ITypeInfo::Invoke %s.%s memid=%d flags=0x%x args=[%s] -> "
                 "member hr=0x%08x result=%s\n",
                 owner_name, fn->name.c_str(), memid, flags, traced_args.c_str(), member_hr,
                 retval_index >= 0 || (!returns_hresult && ret_vt != VT_EMPTY)
                     ? describe_variant(m, retval_store).c_str()
                     : "-");

    if (collection) {
        // Apply the arguments to the returned object's DISPID_VALUE with
        // the caller's flags, LCID of the type information and IID_NULL.
        const u16 vt = m.read_u16(retval_store).value_or(0);
        const GuestAddr disp = static_cast<GuestAddr>(m.read_ptr(retval_store + 8).value_or(0));
        if (vt != VT_DISPATCH || disp == 0 || !m_guest_call) {
            (void)variant_clear_at(m, retval_store);
            return DISP_E_NOTACOLLECTION;
        }
        GuestBlock inner(*this, m, 16 + vsz);
        if (inner.get() == 0) { (void)variant_clear_at(m, retval_store); return E_OUTOFMEMORY_HR; }
        const GuestAddr iid_null = inner.get();
        const GuestAddr inner_result = inner.get() + 16;
        const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(disp).value_or(0));
        const GuestAddr invoke = vtbl == 0 ? 0 : static_cast<GuestAddr>(
            m.read_ptr(vtbl + kDispatchInvokeSlot * ps).value_or(0));
        u32 hr = DISP_E_NOTACOLLECTION;
        if (invoke != 0) {
            const u64 args[9] = {disp, static_cast<u64>(DISPID_VALUE), iid_null,
                                 lib.attributes.lcid, flags, params, inner_result,
                                 excepinfo, argerr};
            hr = static_cast<u32>(m_guest_call(invoke, args, 9, is64));
        }
        (void)variant_clear_at(m, retval_store);
        if (result != 0 && !failed(hr)) {
            std::vector<Byte> bytes(vsz);
            (void)m.memory().read_bytes(inner_result, bytes.data(), vsz);
            (void)m.memory().write_bytes(result, bytes.data(), vsz);
        } else {
            (void)variant_clear_at(m, inner_result);
        }
        return hr;
    }

    // Learn: pVarResult is ignored for property puts.
    if (result != 0 && !is_put) {
        std::vector<Byte> bytes(vsz);
        (void)m.memory().read_bytes(retval_store, bytes.data(), vsz);
        (void)m.memory().write_bytes(result, bytes.data(), vsz);
    } else {
        (void)variant_clear_at(m, retval_store);
    }
    return S_OK;
}

u32 OleAut32Manager::typeinfo_invoke(Marshaler& m) {
    // HRESULT ITypeInfo::Invoke(this, PVOID pvInstance, MEMBERID memid,
    //   WORD wFlags, DISPPARAMS*, VARIANT* pVarResult, EXCEPINFO*, UINT*)
    const GuestAddr object = m.arg_ptr(0);
    auto state = find_typeinfo(object);
    if (state == nullptr || state->parent == nullptr ||
        state->index >= state->parent->typeinfos.size()) {
        return E_INVALIDARG_HR;
    }
    return invoke_member(m, *state, m.arg_ptr(1), static_cast<i32>(m.arg(2)),
                         static_cast<u16>(m.arg(3)), m.arg_ptr(4), m.arg_ptr(5),
                         m.arg_ptr(6), m.arg_ptr(7));
}

u32 OleAut32Manager::disp_invoke(Marshaler& m) {
    // HRESULT DispInvoke(void* _this, ITypeInfo* ptinfo, DISPID dispidMember,
    //   WORD wFlags, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*)
    // Learn IDispatch::Invoke: DispInvoke defers to ITypeInfo::Invoke
    // (measured: `inv dispinvoke` rows equal `inv ti`).
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr typeinfo = m.arg_ptr(1);
    const i32 memid = static_cast<i32>(m.arg(2));
    const u16 flags = static_cast<u16>(m.arg(3));
    if (typeinfo == 0) return E_INVALIDARG_HR;
    if (auto state = find_typeinfo(typeinfo);
        state != nullptr && state->parent != nullptr &&
        state->index < state->parent->typeinfos.size()) {
        return invoke_member(m, *state, self, memid, flags, m.arg_ptr(4), m.arg_ptr(5),
                             m.arg_ptr(6), m.arg_ptr(7));
    }
    // Some other ITypeInfo implementation: call its Invoke.
    if (!m_guest_call) return E_UNEXPECTED_HR;
    const u32 ps = oleaut_ptr_size(m.cpu_is_64bit());
    const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(typeinfo).value_or(0));
    const GuestAddr fn = vtbl == 0 ? 0 : static_cast<GuestAddr>(
        m.read_ptr(vtbl + kTypeInfoInvokeSlot * ps).value_or(0));
    if (fn == 0) return E_INVALIDARG_HR;
    const u64 args[8] = {typeinfo, self, static_cast<u32>(memid), flags,
                         m.arg_ptr(4), m.arg_ptr(5), m.arg_ptr(6), m.arg_ptr(7)};
    return static_cast<u32>(m_guest_call(fn, args, 8, m.cpu_is_64bit()));
}

} // namespace vitreo::winapi
