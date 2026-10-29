// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_propvariant.cpp
/// @brief PROPVARIANT clear and copy (propidl.h).

#include "com_core.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_internal.hpp"

#include <algorithm>
#include <vector>

#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// PROPVARIANT (propidl.h). vt at +0, the value union at +8; 16 bytes on x86,
// 24 on x64. Rules measured in tools/oracle_probe/s7_ole_probe.expected:
//  - PropVariantClear(NULL) is S_OK; an invalid vt returns
//    STG_E_INVALIDPARAMETER and still zeroes the structure; a locked
//    VT_ARRAY returns SafeArrayDestroy's DISP_E_ARRAYISLOCKED, also zeroed.
//  - PropVariantCopy with an invalid vt returns DISP_E_BADVARTYPE and leaves
//    the destination untouched; VT_BYREF is a shallow copy; interfaces are
//    AddRef'd; strings, CLSIDs, blobs and vectors are duplicated.
// =============================================================================
namespace {
constexpr u16 kVtVector = 0x1000, kVtArray = 0x2000, kVtByref = 0x4000;
enum : u16 {
    kVtEmpty = 0, kVtNull = 1, kVtI2 = 2, kVtI4 = 3, kVtR4 = 4, kVtR8 = 5,
    kVtCy = 6, kVtDate = 7, kVtBstr = 8, kVtDispatch = 9, kVtError = 10,
    kVtBool = 11, kVtVariant = 12, kVtUnknown = 13, kVtDecimal = 14,
    kVtI1 = 16, kVtUi1 = 17, kVtUi2 = 18, kVtUi4 = 19, kVtI8 = 20,
    kVtUi8 = 21, kVtInt = 22, kVtUint = 23, kVtLpstr = 30, kVtLpwstr = 31,
    kVtFiletime = 64, kVtBlob = 65, kVtStream = 66, kVtStorage = 67,
    kVtStreamedObject = 68, kVtStoredObject = 69, kVtBlobObject = 70,
    kVtCf = 71, kVtClsid = 72, kVtVersionedStream = 73,
};
constexpr u32 kStgEInvalidParameter = 0x80030057u;
constexpr u32 kDispEBadVarType = 0x80020008u;

/// The PROPVARIANT type table (Learn "PROPVARIANT structure", VARENUM
/// usage): which base types may appear bare, in a VT_VECTOR, VT_ARRAY or
/// VT_BYREF.
bool prop_variant_type_valid(u16 vt) {
    const u16 base = vt & 0x0FFF;
    const u16 modifier = vt & 0xF000;
    auto in = [base](std::initializer_list<u16> set) {
        return std::find(set.begin(), set.end(), base) != set.end();
    };
    switch (modifier) {
    case 0:
        return in({kVtEmpty, kVtNull, kVtI1, kVtUi1, kVtI2, kVtUi2, kVtI4, kVtUi4,
                   kVtInt, kVtUint, kVtI8, kVtUi8, kVtR4, kVtR8, kVtBool, kVtError,
                   kVtCy, kVtDate, kVtFiletime, kVtClsid, kVtCf, kVtBstr, kVtBlob,
                   kVtBlobObject, kVtLpstr, kVtLpwstr, kVtUnknown, kVtDispatch,
                   kVtStream, kVtStreamedObject, kVtStorage, kVtStoredObject,
                   kVtVersionedStream, kVtDecimal});
    case kVtVector:
        return in({kVtI1, kVtUi1, kVtI2, kVtUi2, kVtI4, kVtUi4, kVtI8, kVtUi8,
                   kVtR4, kVtR8, kVtBool, kVtError, kVtCy, kVtDate, kVtFiletime,
                   kVtClsid, kVtCf, kVtBstr, kVtLpstr, kVtLpwstr, kVtVariant});
    case kVtArray:
        return in({kVtI1, kVtUi1, kVtI2, kVtUi2, kVtI4, kVtUi4, kVtInt, kVtUint,
                   kVtR4, kVtR8, kVtBool, kVtDecimal, kVtError, kVtCy, kVtDate,
                   kVtBstr, kVtUnknown, kVtDispatch, kVtVariant});
    case kVtByref:
        return in({kVtI1, kVtUi1, kVtI2, kVtUi2, kVtI4, kVtUi4, kVtInt, kVtUint,
                   kVtR4, kVtR8, kVtBool, kVtDecimal, kVtError, kVtCy, kVtDate,
                   kVtBstr, kVtUnknown, kVtDispatch, kVtArray, kVtVariant});
    default:
        return false;
    }
}

/// Element size of a VT_VECTOR base type (pointer-width for pointers).
u32 vector_element_size(u16 base, bool is64) {
    const u32 ptr = is64 ? 8u : 4u;
    switch (base) {
    case kVtI1: case kVtUi1: return 1;
    case kVtI2: case kVtUi2: case kVtBool: return 2;
    case kVtI4: case kVtUi4: case kVtError: case kVtR4: return 4;
    case kVtI8: case kVtUi8: case kVtR8: case kVtCy: case kVtDate:
    case kVtFiletime: return 8;
    case kVtClsid: return 16;
    case kVtCf: return 8 + ptr;              // CLIPDATA {ULONG, long, BYTE*}
    case kVtBstr: case kVtLpstr: case kVtLpwstr: return ptr;
    case kVtVariant: return is64 ? 24u : 16u;
    default: return 0;
    }
}
} // namespace

u32 ComCoreManager::prop_variant_clear_at(Marshaler& m, GuestAddr pv) {
    if (pv == 0) return com::S_OK;
    const bool is64 = m.cpu_is_64bit();
    const u32 ptr = is64 ? 8u : 4u;
    const u32 size = is64 ? 24u : 16u;
    const GuestAddr value = pv + 8;
    const u16 vt = m.read_u16(pv).value_or(0);
    const u16 base = vt & 0x0FFF;
    auto read_ptr_at = [&](GuestAddr at) {
        return static_cast<GuestAddr>(m.read_ptr(at).value_or(0));
    };
    auto free_task = [&](GuestAddr block) {
        if (block != 0) co_task_mem_free_guest(block);
    };
    // CLIPDATA {ULONG cbSize; long ulClipFmt; BYTE* pClipData}.
    auto free_clipdata_contents = [&](GuestAddr clip) {
        if (clip != 0) free_task(read_ptr_at(clip + 8));
    };

    u32 hr = com::S_OK;
    if (!prop_variant_type_valid(vt)) {
        hr = kStgEInvalidParameter;
    } else if ((vt & kVtByref) != 0) {
        // By-reference values are owned by the caller.
    } else if ((vt & kVtArray) != 0) {
        const GuestAddr array = read_ptr_at(value);
        if (array != 0) {
            hr = static_cast<u32>(call_oleaut32(m, "SafeArrayDestroy", {array}));
        }
    } else if ((vt & kVtVector) != 0) {
        // CAxxx {ULONG cElems; T* pElems}: pElems follows the ULONG, aligned.
        const u32 count = m.read_u32(value).value_or(0);
        const GuestAddr elems = read_ptr_at(value + ptr);
        const u32 elem = vector_element_size(base, is64);
        if (elems != 0) {
            for (u32 i = 0; i < count; ++i) {
                const GuestAddr at = elems + static_cast<GuestAddr>(i) * elem;
                switch (base) {
                case kVtBstr: {
                    const GuestAddr bstr = read_ptr_at(at);
                    if (bstr != 0) (void)call_oleaut32(m, "SysFreeString", {bstr});
                    break;
                }
                case kVtLpstr: case kVtLpwstr:
                    free_task(read_ptr_at(at));
                    break;
                case kVtCf:
                    free_clipdata_contents(at);
                    break;
                case kVtVariant:
                    (void)prop_variant_clear_at(m, at);
                    break;
                default:
                    break;
                }
            }
            free_task(elems);
        }
    } else {
        switch (base) {
        case kVtBstr: {
            const GuestAddr bstr = read_ptr_at(value);
            if (bstr != 0) (void)call_oleaut32(m, "SysFreeString", {bstr});
            break;
        }
        case kVtLpstr: case kVtLpwstr: case kVtClsid:
            free_task(read_ptr_at(value));
            break;
        case kVtBlob: case kVtBlobObject:
            // BLOB {ULONG cbSize; BYTE* pBlobData}.
            free_task(read_ptr_at(value + ptr));
            break;
        case kVtCf: {
            const GuestAddr clip = read_ptr_at(value);
            free_clipdata_contents(clip);
            free_task(clip);
            break;
        }
        case kVtUnknown: case kVtDispatch: case kVtStream: case kVtStorage:
        case kVtStreamedObject: case kVtStoredObject:
            guest_iunknown_release(m, read_ptr_at(value));
            break;
        case kVtVersionedStream: {
            // VERSIONEDSTREAM {GUID guidVersion; IStream* pStream}.
            const GuestAddr versioned = read_ptr_at(value);
            if (versioned != 0) {
                guest_iunknown_release(m, read_ptr_at(versioned + 16));
                free_task(versioned);
            }
            break;
        }
        default:
            break;
        }
    }
    std::vector<Byte> zeros(size, 0);
    (void)m.memory().write_bytes(pv, zeros.data(), zeros.size());
    return hr;
}

u32 ComCoreManager::prop_variant_copy_at(Marshaler& m, GuestAddr dst, GuestAddr src) {
    if (dst == 0 || src == 0) return com::E_INVALIDARG;
    const bool is64 = m.cpu_is_64bit();
    const u32 ptr = is64 ? 8u : 4u;
    const u32 size = is64 ? 24u : 16u;
    auto vt_r = m.read_u16(src);
    if (vt_r.is_err()) return com::E_INVALIDARG;
    const u16 vt = vt_r.value();
    if (!prop_variant_type_valid(vt)) return kDispEBadVarType;
    const u16 base = vt & 0x0FFF;

    std::vector<Byte> bytes(size);
    if (m.memory().read_bytes(src, bytes.data(), size).is_err()) return com::E_INVALIDARG;
    const GuestAddr src_value = src + 8;
    auto read_ptr_at = [&](GuestAddr at) {
        return static_cast<GuestAddr>(m.read_ptr(at).value_or(0));
    };
    // Duplicate `length` bytes of guest memory into fresh task memory.
    auto dup = [&](GuestAddr from, u64 length) -> GuestAddr {
        const GuestAddr to = co_task_mem_alloc_guest(length == 0 ? 1 : length);
        if (to == 0) return 0;
        std::vector<Byte> tmp(static_cast<usize>(length));
        if (length != 0 &&
            (m.memory().read_bytes(from, tmp.data(), tmp.size()).is_err() ||
             m.memory().write_bytes(to, tmp.data(), tmp.size()).is_err())) {
            co_task_mem_free_guest(to);
            return 0;
        }
        return to;
    };
    auto string_bytes = [&](GuestAddr from, bool wide) -> u64 {
        u64 n = 0;
        if (wide) { while (m.read_u16(from + n * 2).value_or(0) != 0) ++n; return (n + 1) * 2; }
        while (m.read_u8(from + n).value_or(0) != 0) ++n;
        return n + 1;
    };
    auto dup_bstr = [&](GuestAddr bstr) -> GuestAddr {
        if (bstr == 0) return 0;
        const u32 byte_len = m.read_u32(bstr - 4).value_or(0);
        return static_cast<GuestAddr>(
            call_oleaut32(m, "SysAllocStringByteLen", {bstr, byte_len}));
    };
    auto dup_clipdata_into = [&](GuestAddr from, GuestAddr to) -> bool {
        // CLIPDATA.cbSize counts ulClipFmt plus the data (Learn CLIPDATA).
        const u32 cb = m.read_u32(from).value_or(0);
        const GuestAddr data = read_ptr_at(from + 8);
        (void)m.write_u32(to, cb);
        (void)m.write_u32(to + 4, m.read_u32(from + 4).value_or(0));
        GuestAddr copy = 0;
        if (data != 0 && cb > 4) {
            copy = dup(data, cb - 4);
            if (copy == 0) return false;
        }
        (void)m.write_ptr(to + 8, copy);
        return true;
    };

    // Build the copy in a host buffer so a failure leaves dst untouched.
    auto put_ptr = [&](usize offset, u64 value) {
        for (u32 i = 0; i < ptr; ++i) bytes[offset + i] = static_cast<Byte>(value >> (8 * i));
    };
    if ((vt & kVtByref) != 0) {
        // Shallow: the referenced storage stays the caller's.
    } else if ((vt & kVtArray) != 0) {
        const GuestAddr array = read_ptr_at(src_value);
        if (array != 0) {
            const GuestAddr slot = co_task_mem_alloc_guest(8);
            if (slot == 0) return kEOutOfMemory;
            (void)m.write_ptr(slot, 0);
            const u32 hr = static_cast<u32>(call_oleaut32(m, "SafeArrayCopy", {array, slot}));
            const GuestAddr copy = read_ptr_at(slot);
            co_task_mem_free_guest(slot);
            if ((hr & 0x80000000u) != 0) return hr;
            put_ptr(8, copy);
        }
    } else if ((vt & kVtVector) != 0) {
        const u32 count = m.read_u32(src_value).value_or(0);
        const GuestAddr elems = read_ptr_at(src_value + ptr);
        const u32 elem = vector_element_size(base, is64);
        GuestAddr copy = 0;
        if (elems != 0 && count != 0) {
            copy = dup(elems, static_cast<u64>(count) * elem);
            if (copy == 0) return kEOutOfMemory;
            for (u32 i = 0; i < count; ++i) {
                const GuestAddr from = elems + static_cast<GuestAddr>(i) * elem;
                const GuestAddr to = copy + static_cast<GuestAddr>(i) * elem;
                GuestAddr item = 0;
                switch (base) {
                case kVtBstr:
                    item = dup_bstr(read_ptr_at(from));
                    (void)m.write_ptr(to, item);
                    break;
                case kVtLpstr: case kVtLpwstr: {
                    const GuestAddr str = read_ptr_at(from);
                    item = str == 0 ? 0 : dup(str, string_bytes(str, base == kVtLpwstr));
                    (void)m.write_ptr(to, item);
                    break;
                }
                case kVtCf:
                    (void)dup_clipdata_into(from, to);
                    break;
                case kVtVariant:
                    (void)prop_variant_copy_at(m, to, from);
                    break;
                default:
                    break;
                }
            }
        }
        put_ptr(8 + ptr, copy);
    } else {
        GuestAddr payload = read_ptr_at(src_value);
        switch (base) {
        case kVtBstr:
            put_ptr(8, dup_bstr(payload));
            break;
        case kVtLpstr: case kVtLpwstr:
            if (payload != 0) {
                const GuestAddr copy = dup(payload, string_bytes(payload, base == kVtLpwstr));
                if (copy == 0) return kEOutOfMemory;
                put_ptr(8, copy);
            }
            break;
        case kVtClsid:
            if (payload != 0) {
                const GuestAddr copy = dup(payload, 16);
                if (copy == 0) return kEOutOfMemory;
                put_ptr(8, copy);
            }
            break;
        case kVtBlob: case kVtBlobObject: {
            const u32 cb = m.read_u32(src_value).value_or(0);
            const GuestAddr data = read_ptr_at(src_value + ptr);
            if (data != 0 && cb != 0) {
                const GuestAddr copy = dup(data, cb);
                if (copy == 0) return kEOutOfMemory;
                put_ptr(8 + ptr, copy);
            }
            break;
        }
        case kVtCf:
            if (payload != 0) {
                const GuestAddr clip = co_task_mem_alloc_guest(8 + ptr);
                if (clip == 0 || !dup_clipdata_into(payload, clip)) return kEOutOfMemory;
                put_ptr(8, clip);
            }
            break;
        case kVtUnknown: case kVtDispatch: case kVtStream: case kVtStorage:
        case kVtStreamedObject: case kVtStoredObject:
            if (payload != 0) (void)guest_iunknown_addref(m, payload);
            break;
        case kVtVersionedStream:
            if (payload != 0) {
                const GuestAddr copy = dup(payload, 16 + ptr);
                if (copy == 0) return kEOutOfMemory;
                (void)guest_iunknown_addref(m, read_ptr_at(copy + 16));
                put_ptr(8, copy);
            }
            break;
        default:
            break;
        }
    }
    if (m.memory().write_bytes(dst, bytes.data(), size).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::prop_variant_copy(Marshaler& m) {
    return prop_variant_copy_at(m, m.arg_ptr(0), m.arg_ptr(1));
}

} // namespace vitreo::winapi
