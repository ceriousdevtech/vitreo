// SPDX-License-Identifier: LGPL-2.1-only
/// @file oleaut32.cpp
/// @brief OLEAUT32.dll core: task-allocator storage, BSTR and VARIANT
///        lifetime (init/clear/copy) and the IUnknown ref helpers they use.
///
/// Type coercion, SAFEARRAY, the ordinal-only exports and the NDR wire
/// marshaling live in the sibling oleaut32_*.cpp files.
///
/// BSTR memory layout: [4-byte length][UTF-16 chars][2-byte null terminator]
/// The returned pointer points to the first character, not the length prefix.

#include "oleaut32.hpp"
#include "winapi/com/oleaut32/oleaut32_internal2.hpp"
#include "winapi/com/oleaut32/oleaut32_internal.hpp"
#include "winapi/com/oleaut32/oleaut32_shared_internal.hpp"
#include "winapi/com/ole32/task_memory.hpp"
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


// =============================================================================
// OleAut32Manager Implementation
// =============================================================================

GuestAddr OleAut32Manager::scratch_alloc(u32 size) noexcept {
    size = (size + 3) & ~3u; // 4-byte alignment
    if (size == 0) size = 4;
    if (size > OLEAUT32_SCRATCH_ALLOC_MAX) {
        std::fprintf(stderr,
            "[oleaut32] scratch_alloc rejected oversized need=%u (max=%u)\n",
            size, OLEAUT32_SCRATCH_ALLOC_MAX);
        return 0;
    }
    std::lock_guard lock(m_scratch_mutex);
    if (m_scratch_offset + size > OLEAUT32_SCRATCH_SIZE) {
        std::fprintf(stderr,
            "[oleaut32] scratch_alloc OOM: need=%u used=%u/%u\n",
            size, m_scratch_offset, OLEAUT32_SCRATCH_SIZE);
        return 0;
    }
    GuestAddr addr = OLEAUT32_SCRATCH_BASE + m_scratch_offset;
    // Commit only as far as the bump has actually reached: the range is
    // RESERVED at registration so a guest that never touches oleaut32 pays
    // nothing for it (committing all 256 MB up front was the dirty-memory
    // floor of every guest process).
    if (m_vmm != nullptr) {
        const u64 needed = static_cast<u64>(m_scratch_offset) + size;
        if (needed > m_scratch_committed) {
            constexpr u64 kChunk = 1ull << 20;   // 1 MB
            const u64 target =
                std::min<u64>(OLEAUT32_SCRATCH_SIZE,
                              ((needed + kChunk - 1) / kChunk) * kChunk);
            const u64 base = OLEAUT32_SCRATCH_BASE + m_scratch_committed;
            const u64 bytes = target - m_scratch_committed;
            constexpr u32 kCommit = 0x1000;   // MEM_COMMIT
            constexpr u32 kRW = 0x04;         // PAGE_READWRITE
            if (m_vmm->virtual_alloc(base, bytes, kCommit, kRW) == 0) {
                std::fprintf(stderr,
                    "[oleaut32] scratch commit FAILED base=0x%llx bytes=0x%llx\n",
                    static_cast<unsigned long long>(base),
                    static_cast<unsigned long long>(bytes));
                return 0;
            }
            m_scratch_committed = target;
        }
    }
    m_scratch_offset += size;
    return addr;
}

// -----------------------------------------------------------------------------
// Task-allocator storage for BSTRs and SAFEARRAYs.
//
// Windows oleaut32 allocates both through the COM task allocator (IMalloc),
// so SysFreeString / SafeArrayDestroy really return the memory. With no task
// allocator wired (bare unit tests) the scratch bump is used and nothing is
// ever reclaimed; that fallback never runs in a guest process.
// -----------------------------------------------------------------------------

GuestAddr OleAut32Manager::oa_alloc(Marshaler& m, u64 size) {
    if (size == 0) size = 1;
    GuestAddr block = 0;
    if (m_task_memory != nullptr) {
        block = m_task_memory->allocate(size);
    } else {
        if (size > OLEAUT32_SCRATCH_ALLOC_MAX) return 0;
        // Scratch blocks are 4-aligned; round the start up to 16 so the
        // x64 layouts below match what HeapAlloc returns.
        const GuestAddr raw = scratch_alloc(static_cast<u32>(size + 12));
        block = raw == 0 ? 0 : ((raw + 15) & ~GuestAddr{15});
    }
    if (block == 0) return 0;
    // Zero-fill in bounded chunks (a SAFEARRAY data block can be large).
    static constexpr usize kZero = 64 * 1024;
    static const std::vector<Byte> zeros(kZero, 0);
    for (u64 off = 0; off < size; off += kZero) {
        const u64 n = std::min<u64>(kZero, size - off);
        if (m.memory().write_bytes(block + off, zeros.data(), n).is_err()) {
            oa_free(block);
            return 0;
        }
    }
    return block;
}

void OleAut32Manager::oa_free(GuestAddr block) {
    if (block == 0 || m_task_memory == nullptr) return;
    if (m_task_memory->owns(block)) {
        m_task_memory->free(block);
    }
}

namespace {

// BSTR block layout. The four-byte byte-length prefix sits directly before
// the first character. Win64 oleaut32 pads the header so the string itself
// is 8-byte aligned (oleaut32_safearray_probe: BSTR & 7 == 0 on x64, == 4 on
// x86), i.e. [pad 4][len 4][chars][NUL 2] on x64 and [len 4][chars][NUL 2]
// on x86.
[[nodiscard]] u32 bstr_header_size(bool is64) noexcept { return is64 ? 8u : 4u; }

} // namespace

GuestAddr OleAut32Manager::bstr_alloc_bytes(Marshaler& m, GuestAddr src,
                                            u64 byte_len) {
    if (byte_len > kBstrMaxBytes) return 0;
    const bool is64 = m.cpu_is_64bit();
    const u32 hdr = bstr_header_size(is64);
    const GuestAddr block = oa_alloc(m, hdr + byte_len + 2);
    if (block == 0) return 0;
    const GuestAddr bstr = block + hdr;
    if (m.write_u32(bstr - 4, static_cast<u32>(byte_len)).is_err()) {
        oa_free(block);
        return 0;
    }
    if (src != 0 && byte_len != 0) {
        static constexpr u64 kChunk = 64 * 1024;
        std::vector<Byte> buf(static_cast<usize>(std::min(kChunk, byte_len)));
        for (u64 off = 0; off < byte_len; off += kChunk) {
            const u64 n = std::min(kChunk, byte_len - off);
            if (m.memory().read_bytes(src + off, buf.data(), n).is_err() ||
                m.memory().write_bytes(bstr + off, buf.data(), n).is_err()) {
                oa_free(block);
                return 0;
            }
        }
    }
    // The terminator is two zero bytes directly after the data, even for an
    // odd byte length (probe bstr_bytelen: bytes 5 and 6 are both 0).
    // oa_alloc zero-filled the block, so they are already in place.
    return bstr;
}

void OleAut32Manager::bstr_free(Marshaler& m, GuestAddr bstr) {
    if (bstr == 0) return;
    oa_free(bstr - bstr_header_size(m.cpu_is_64bit()));
}

GuestAddr OleAut32Manager::sys_alloc_string(Marshaler& m) {
    return sys_alloc_string_from_z(m, m.arg_ptr(0));
}

GuestAddr OleAut32Manager::sys_alloc_string_from_z(Marshaler& m, GuestAddr psz) {
    if (psz == 0) return 0; // SysAllocString(NULL) returns NULL.
    u64 cch = 0;
    while (cch * 2 <= kBstrMaxBytes) {
        const auto ch = m.read_u16(psz + cch * 2);
        if (ch.is_err() || ch.value() == 0) break;
        ++cch;
    }
    return bstr_alloc_bytes(m, psz, cch * 2);
}

GuestAddr OleAut32Manager::sys_alloc_string_len(Marshaler& m, GuestAddr psz, u32 cch) {
    return bstr_alloc_bytes(m, psz, static_cast<u64>(cch) * 2);
}

GuestAddr OleAut32Manager::sys_alloc_string_byte_len(Marshaler& m, GuestAddr psz, u32 len) {
    return bstr_alloc_bytes(m, psz, len);
}

GuestAddr OleAut32Manager::allocate_bstr_bytes(
    Marshaler& m, std::span<const Byte> utf16_bytes) {
    if ((utf16_bytes.size() & 1u) != 0) return 0;
    const GuestAddr bstr = bstr_alloc_bytes(m, 0, utf16_bytes.size());
    if (bstr == 0) return 0;
    if (!utf16_bytes.empty() &&
        m.memory().write_bytes(bstr, utf16_bytes.data(),
                               utf16_bytes.size()).is_err()) {
        bstr_free(m, bstr);
        return 0;
    }
    return bstr;
}

void OleAut32Manager::sys_free_string(Marshaler& m, GuestAddr bstr) {
    bstr_free(m, bstr);
}

u32 OleAut32Manager::sys_string_len(Marshaler& m, GuestAddr bstr) {
    if (bstr == 0) return 0;
    u32 byte_len = m.read_u32(bstr - 4).value_or(0);
    return byte_len / 2;
}

u32 OleAut32Manager::sys_string_byte_len(Marshaler& m, GuestAddr bstr) {
    if (bstr == 0) return 0;
    return m.read_u32(bstr - 4).value_or(0);
}

u32 OleAut32Manager::sys_realloc_string_len(Marshaler& m) {
    // INT SysReAllocStringLen(BSTR* pbstr, const OLECHAR* psz, unsigned len)
    const GuestAddr pbstr = m.arg_ptr(0);
    const GuestAddr psz   = m.arg_ptr(1);
    const u32 cch         = static_cast<u32>(m.arg(2));
    if (pbstr == 0) return 0;
    // Allocate first: psz may point into the old BSTR (Learn).
    const GuestAddr old_bstr =
        static_cast<GuestAddr>(m.read_ptr(pbstr).value_or(0));
    const GuestAddr new_bstr = sys_alloc_string_len(m, psz, cch);
    if (new_bstr == 0) return 0; // FALSE: insufficient memory, *pbstr kept
    if (m.write_ptr(pbstr, new_bstr).is_err()) {
        bstr_free(m, new_bstr);
        return 0;
    }
    if (old_bstr != 0 && old_bstr != new_bstr) {
        bstr_free(m, old_bstr);
    }
    return 1; // TRUE
}

void OleAut32Manager::variant_init(Marshaler& m) {
    GuestAddr pvar = m.arg_ptr(0);
    if (pvar == 0) return;
    const u32 vsz = oleaut_variant_size(m.cpu_is_64bit());
    u8 zeros[24] = {};
    (void)m.memory().write_bytes(pvar, zeros, vsz);
}

void OleAut32Manager::com_add_ref(Marshaler& m, GuestAddr punk) {
    if (punk == 0) return;
    if (!m_guest_call) {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED oleaut32!IUnknown::AddRef: no guest-call "
            "bridge wired (object 0x%llx)\n",
            static_cast<unsigned long long>(punk));
        return;
    }
    const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(punk).value_or(0));
    const GuestAddr fn = vtbl == 0 ? 0 : static_cast<GuestAddr>(
        m.read_ptr(vtbl + oleaut_ptr_size(m.cpu_is_64bit())).value_or(0));
    if (fn == 0) return;
    const u64 args[1] = {punk};
    (void)m_guest_call(fn, args, 1, m.cpu_is_64bit());
}

void OleAut32Manager::com_release(Marshaler& m, GuestAddr punk) {
    if (punk == 0) return;
    if (!m_guest_call) {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED oleaut32!IUnknown::Release: no guest-call "
            "bridge wired (object 0x%llx)\n",
            static_cast<unsigned long long>(punk));
        return;
    }
    const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(punk).value_or(0));
    const GuestAddr fn = vtbl == 0 ? 0 : static_cast<GuestAddr>(
        m.read_ptr(vtbl + 2 * oleaut_ptr_size(m.cpu_is_64bit())).value_or(0));
    if (fn == 0) return;
    const u64 args[1] = {punk};
    (void)m_guest_call(fn, args, 1, m.cpu_is_64bit());
}

namespace {
constexpr u16 kVtArray   = 0x2000;
constexpr u32 kDispEBadVarType = 0x80020008u;
constexpr u32 kEOutOfMemory    = 0x8007000Eu;
} // namespace

u32 OleAut32Manager::variant_clear_at(Marshaler& m, GuestAddr pvar) {
    if (pvar == 0) return 0x80070057u; // E_INVALIDARG
    const u32 vsz = oleaut_variant_size(m.cpu_is_64bit());
    const u16 vt = m.read_u16(pvar).value_or(0);
    // VT_BYREF variants do not own what they point at.
    if ((vt & VT_BYREF) == 0) {
        const GuestAddr payload =
            static_cast<GuestAddr>(m.read_ptr(pvar + 8).value_or(0));
        if ((vt & kVtArray) != 0) {
            if (payload != 0) {
                const u32 hr = sa_destroy(m, payload);
                if (hr != S_OK) return hr; // e.g. DISP_E_ARRAYISLOCKED
            }
        } else if (vt == VT_BSTR) {
            bstr_free(m, payload);
        } else if (vt == VT_UNKNOWN || vt == VT_DISPATCH) {
            com_release(m, payload);
        } else if (vt == kVtRecord) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED oleaut32!VariantClear: VT_RECORD "
                    "payload not released\n");
            }
        }
    }
    u8 zeros[24] = {};
    (void)m.memory().write_bytes(pvar, zeros, vsz);
    return S_OK;
}

u32 OleAut32Manager::variant_copy_at(Marshaler& m, GuestAddr dst, GuestAddr src) {
    if (dst == 0 || src == 0) return 0x80070057u;
    const u32 vsz = oleaut_variant_size(m.cpu_is_64bit());
    u8 src_buf[24] = {};
    (void)m.memory().read_bytes(src, src_buf, vsz);
    if (dst == src) return S_OK;
    const u32 clear_hr = variant_clear_at(m, dst);
    if (clear_hr != S_OK) return clear_hr;

    const u16 vt = static_cast<u16>(src_buf[0] | (src_buf[1] << 8));
    GuestAddr payload = 0;
    std::memcpy(&payload, &src_buf[8], m.cpu_is_64bit() ? 8 : 4);
    if ((vt & VT_BYREF) == 0) {
        if ((vt & kVtArray) != 0) {
            GuestAddr copy = 0;
            if (payload != 0) {
                const u32 hr = sa_copy(m, payload, copy);
                if (hr != S_OK) return hr;
            }
            (void)m.memory().write_bytes(dst, src_buf, vsz);
            (void)m.write_ptr(dst + 8, copy);
            return S_OK;
        }
        if (vt == VT_BSTR) {
            GuestAddr copy = 0;
            if (payload != 0) {
                copy = bstr_alloc_bytes(m, payload,
                                        m.read_u32(payload - 4).value_or(0));
                if (copy == 0) return kEOutOfMemory;
            }
            (void)m.memory().write_bytes(dst, src_buf, vsz);
            (void)m.write_ptr(dst + 8, copy);
            return S_OK;
        }
        if (vt == VT_UNKNOWN || vt == VT_DISPATCH) {
            com_add_ref(m, payload);
        } else if (vt == kVtRecord) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED oleaut32!VariantCopy: VT_RECORD "
                    "copied shallow\n");
            }
        } else if ((vt & 0x0FFF) > 0x4F && (vt & 0x0FFF) != 0xFFF) {
            return kDispEBadVarType;
        }
    }
    (void)m.memory().write_bytes(dst, src_buf, vsz);
    return S_OK;
}

u32 OleAut32Manager::variant_clear(Marshaler& m) {
    return variant_clear_at(m, m.arg_ptr(0));
}

u32 OleAut32Manager::variant_copy(Marshaler& m) {
    return variant_copy_at(m, m.arg_ptr(0), m.arg_ptr(1));
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void oleaut32::impl_SysAllocString(Marshaler& m, OleAut32Manager& mgr) {
    m.set_return(mgr.sys_alloc_string(m));
}

void oleaut32::impl_SysAllocStringLen(Marshaler& m, OleAut32Manager& mgr) {
    m.set_return(mgr.sys_alloc_string_len(m, m.arg_ptr(0),
                                          static_cast<u32>(m.arg(1))));
}

void oleaut32::impl_SysAllocStringByteLen(Marshaler& m, OleAut32Manager& mgr) {
    m.set_return(mgr.sys_alloc_string_byte_len(m, m.arg_ptr(0),
                                               static_cast<u32>(m.arg(1))));
}

void oleaut32::impl_SysFreeString(Marshaler& m, OleAut32Manager& mgr) {
    mgr.sys_free_string(m, m.arg_ptr(0));
    m.set_return(0);
}

void oleaut32::impl_SysStringLen(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.sys_string_len(m, m.arg_ptr(0));
    m.set_return(rc);
}

void oleaut32::impl_SysStringByteLen(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.sys_string_byte_len(m, m.arg_ptr(0));
    m.set_return(rc);
}

void oleaut32::impl_SysReAllocStringLen(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.sys_realloc_string_len(m);
    m.set_return(rc);
}

/// BOOL SysReAllocString(BSTR *pbstr, const OLECHAR *psz)
///
/// Same contract as SysReAllocStringLen except the length comes from the
/// source string's NUL terminator rather than from the caller.
void oleaut32::impl_SysReAllocString(Marshaler& m, OleAut32Manager& mgr) {
    const GuestAddr pbstr = m.arg_ptr(0);
    const GuestAddr psz   = m.arg_ptr(1);
    if (pbstr == 0) {
        m.set_return(0u); // FALSE
        return;
    }
    const GuestAddr new_bstr = mgr.sys_alloc_string_from_z(m, psz);
    if (new_bstr == 0 && psz != 0) {
        m.set_return(0u); // insufficient memory: *pbstr unchanged
        return;
    }
    // psz may point into the old BSTR, so free it only after the copy.
    const GuestAddr old = static_cast<GuestAddr>(m.read_ptr(pbstr).value_or(0));
    if (m.write_ptr(pbstr, new_bstr).is_err()) {
        mgr.bstr_free(m, new_bstr);
        m.set_return(0u);
        return;
    }
    if (old != 0 && old != new_bstr) mgr.bstr_free(m, old);
    m.set_return(1u); // TRUE
}

void oleaut32::impl_VariantInit(Marshaler& m, OleAut32Manager& mgr) {
    mgr.variant_init(m);
    // VariantInit has no return value (void)
    m.set_return(0);
}

void oleaut32::impl_VariantClear(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.variant_clear(m);
    m.set_return(rc);
}

void oleaut32::impl_VariantCopy(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.variant_copy(m);
    m.set_return(rc);
}

} // namespace vitreo::winapi
