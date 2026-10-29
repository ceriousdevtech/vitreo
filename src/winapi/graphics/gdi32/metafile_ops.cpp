// SPDX-License-Identifier: LGPL-2.1-only

/// @file metafile_ops.cpp
/// @brief Enhanced metafiles held as their records.
///
/// SetEnhMetaFileBits / GetEnhMetaFileBits / GetEnhMetaFileHeader /
/// EnumEnhMetaFile / DeleteEnhMetaFile work on the record stream itself
/// (Learn: ENHMETAHEADER, the EMR_HEADER record that starts every enhanced
/// metafile with the " EMF" signature and the file's size). Recording
/// (CreateEnhMetaFile) and playback (PlayEnhMetaFile / PlayEnhMetaFileRecord)
/// need a GDI command journal and an interpreter for every record type;
/// Vitreo has neither, so those fail loudly (gdi32_registration.cpp).
/// tools/oracle_probe/s3gdi_print_probe.c measured the failure contracts:
/// junk bits and NULL handles fail with the last error untouched.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi {

namespace {
u32 le32(const std::vector<u8>& b, usize o) {
    u32 v = 0;
    if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4);
    return v;
}
}  // namespace

Handle GdiManager::set_enh_metafile_bits(std::span<const u8> data) {
    ApiLock api_lock(*this);
    // EMR_HEADER (type 1) first, " EMF" at offset 40, nBytes at 48 within the
    // data; anything else is not a metafile ("SetEnhMetaFileBits(junk)=NULL").
    std::vector<u8> bytes(data.begin(), data.end());
    if (bytes.size() < 88 || le32(bytes, 0) != 1 || le32(bytes, 40) != 0x464D4520u) return 0;
    const u32 total = le32(bytes, 48);
    if (total < 88 || total > bytes.size()) return 0;
    bytes.resize(total);
    // Every record must fit and be DWORD sized.
    for (usize at = 0; at < bytes.size();) {
        const u32 size = le32(bytes, at + 4);
        if (size < 8 || (size & 3) != 0 || at + size > bytes.size()) return 0;
        at += size;
    }
    const Handle h = allocate_gdi_handle(kHandleTypeMetaFile);
    m_enh_metafiles[h] = std::move(bytes);
    return h;
}

const std::vector<u8>* GdiManager::enh_metafile(Handle h) const {
    ApiLock api_lock(*this);
    const auto it = m_enh_metafiles.find(h);
    return it == m_enh_metafiles.end() ? nullptr : &it->second;
}

bool GdiManager::delete_enh_metafile(Handle h) {
    ApiLock api_lock(*this);
    // "DeleteEnhMetaFile(NULL)=0 err=57005".
    if (m_enh_metafiles.erase(h) == 0) return false;
    (void)m_handles.close(h);
    return true;
}

i32 GdiManager::enum_enh_metafile(Handle hdc, Handle emf, GuestAddr proc, u64 data, bool is_64bit) {
    ApiLock api_lock(*this);
    // The callback sees each record in order with a handle table whose first
    // entry is the metafile ("record type=1 size=108 nobj=1" ...); a zero
    // return stops the enumeration and is returned.
    const auto* stored = enh_metafile(emf);
    if (stored == nullptr || proc == 0 || !m_guest_call || !m_guest_memory) return 0;
    const std::vector<u8> snapshot = *stored;
    const auto* bytes = &snapshot;
    const auto caller = m_guest_call;
    const u32 handles = std::max<u32>(1, static_cast<u32>(le32(*bytes, 56) & 0xFFFF));
    const u32 ptr = is_64bit ? 8u : 4u;
    const u32 table_size = handles * ptr;
    const u32 total = static_cast<u32>(bytes->size()) + table_size + 16;
    const GuestAddr block = dib_scratch_alloc(total);
    if (block == 0) return 0;
    const GuestAddr records = block;
    const GuestAddr table = block + ((bytes->size() + 7) & ~usize{7});
    std::vector<u8> tbl(table_size, 0);
    const u64 self = emf;
    std::memcpy(tbl.data(), &self, ptr);
    i32 result = 1;
    if (m_guest_memory->write_bytes(records, bytes->data(), bytes->size()).is_err() ||
        m_guest_memory->write_bytes(table, tbl.data(), tbl.size()).is_err()) {
        result = 0;
    } else {
        for (usize at = 0; at < bytes->size();) {
            const u32 size = le32(*bytes, at + 4);
            const u64 args[5] = {hdc, table, records + at, handles, data};
            u64 callback_result = 0;
            {
                ApiUnlock unlocked(*this);
                callback_result = caller(proc, args, 5, is_64bit);
            }
            if (static_cast<i32>(callback_result) == 0) {
                result = 0;
                break;
            }
            at += size;
        }
    }
    dib_scratch_free(block, total);
    return result;
}

}  // namespace vitreo::winapi
