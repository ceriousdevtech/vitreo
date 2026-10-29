// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/media/mfplat/mf_attributes.hpp"

#include <cstdio>
#include <cstring>

namespace vitreo::winapi {

namespace {

using GuidBytes = std::array<u8, 16>;

// SDK mfobjects.h MIDL_INTERFACE ids, in guest memory order.
inline constexpr GuidBytes IID_IUNKNOWN{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
inline constexpr GuidBytes IID_IMFATTRIBUTES{
    0x21, 0xd9, 0xd2, 0x2c, 0x47, 0xc4, 0xa7, 0x44,
    0xa1, 0x3c, 0x4a, 0xda, 0xbf, 0xc2, 0x47, 0xe3};
inline constexpr GuidBytes IID_IMFMEDIATYPE{
    0xa8, 0x0f, 0xae, 0x44, 0x31, 0xea, 0x09, 0x41,
    0x8d, 0x2e, 0x4c, 0xae, 0x49, 0x97, 0xc5, 0x55};

// SDK mfapi.h DEFINE_GUIDs the IMFMediaType methods read.
constexpr MfGuid guid(u32 a, u16 b, u16 c, u8 d0, u8 d1, u8 d2, u8 d3,
                      u8 d4, u8 d5, u8 d6, u8 d7) {
    return MfGuid{a, b, c, {d0, d1, d2, d3, d4, d5, d6, d7}};
}
constexpr MfGuid MF_MT_MAJOR_TYPE = guid(0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f);
constexpr MfGuid MF_MT_SUBTYPE = guid(0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5);
constexpr MfGuid MF_MT_ALL_SAMPLES_INDEPENDENT = guid(0xc9173739, 0x5e56, 0x461c, 0xb7, 0x13, 0x46, 0xfb, 0x99, 0x5c, 0xb9, 0x5f);
constexpr MfGuid MF_MT_USER_DATA = guid(0xb6bc765f, 0x4c3b, 0x40a4, 0xbd, 0x51, 0x25, 0x35, 0xb6, 0x6f, 0xe0, 0x9d);
constexpr MfGuid MF_MT_FRAME_RATE_RANGE_MIN = guid(0xd2e7558c, 0xdc1f, 0x403f, 0x9a, 0x72, 0xd2, 0x8b, 0xb1, 0xeb, 0x3b, 0x5e);
constexpr MfGuid MF_MT_FRAME_RATE_RANGE_MAX = guid(0xe3371d41, 0xb4cf, 0x4a05, 0xbd, 0x4e, 0x20, 0xb8, 0x8b, 0xb2, 0xc4, 0xd6);

MfGuid from_bytes(const GuidBytes& b) {
    MfGuid g;
    std::memcpy(&g, b.data(), 16);
    return g;
}

// HRESULTs (winerror.h, mferror.h).
constexpr u32 S_OK_HR = 0x00000000u;
constexpr u32 S_FALSE_HR = 0x00000001u;
constexpr u32 E_NOTIMPL_HR = 0x80004001u;
constexpr u32 E_NOINTERFACE_HR = 0x80004002u;
constexpr u32 E_POINTER_HR = 0x80004003u;
constexpr u32 E_INVALIDARG_HR = 0x80070057u;
constexpr u32 E_OUTOFMEMORY_HR = 0x8007000Eu;
constexpr u32 E_NOT_SUFFICIENT_BUFFER_HR = 0x8007007Au;
constexpr u32 MF_E_ATTRIBUTENOTFOUND_HR = 0xC00D36E6u;
constexpr u32 MF_E_INVALIDTYPE_HR = 0xC00D36BDu;
constexpr u32 MF_E_BAD_STARTUP_VERSION_HR = 0xC00D36E3u;
constexpr u32 MF_E_UNSUPPORTED_REPRESENTATION_HR = 0xC00D36B7u;

// PROPVARIANT vt values (propidl.h) for the MF_ATTRIBUTE_TYPE set.
constexpr u16 VT_EMPTY_T = 0;
constexpr u16 VT_R8_T = 5;
constexpr u16 VT_UNKNOWN_T = 13;
constexpr u16 VT_UI4_T = 19;
constexpr u16 VT_UI8_T = 21;
constexpr u16 VT_LPWSTR_T = 31;
constexpr u16 VT_CLSID_T = 72;
constexpr u16 VT_BLOB_T = 0x1011; // VT_VECTOR | VT_UI1 (MF_ATTRIBUTE_BLOB)

constexpr u32 MF_SDK_VERSION = 2; // SDK mfapi.h (WINVER >= Win7)

// IMFMediaType::IsEqual flags (mfobjects.h).
constexpr u32 MF_MEDIATYPE_EQUAL_MAJOR_TYPES = 0x1;
constexpr u32 MF_MEDIATYPE_EQUAL_FORMAT_TYPES = 0x2;
constexpr u32 MF_MEDIATYPE_EQUAL_FORMAT_DATA = 0x4;
constexpr u32 MF_MEDIATYPE_EQUAL_FORMAT_USER_DATA = 0x8;

// IMFAttributes::Compare match types (mfobjects.h MF_ATTRIBUTES_MATCH_TYPE).
constexpr u32 MF_ATTRIBUTES_MATCH_OUR_ITEMS = 0;
constexpr u32 MF_ATTRIBUTES_MATCH_THEIR_ITEMS = 1;
constexpr u32 MF_ATTRIBUTES_MATCH_ALL_ITEMS = 2;
constexpr u32 MF_ATTRIBUTES_MATCH_INTERSECTION = 3;
constexpr u32 MF_ATTRIBUTES_MATCH_SMALLER = 4;

constexpr u32 kAttributesSlots = 33;
constexpr u32 kMediaTypeSlots = 38;

// x86 stdcall bytes per slot: `this` plus the SDK parameter list.
constexpr std::array<u8, kMediaTypeSlots> kStdcallBytes{{
    12, 4, 4,            // QueryInterface, AddRef, Release
    12, 12, 16, 16,      // GetItem, GetItemType, CompareItem, Compare
    12, 12, 12, 12,      // GetUINT32, GetUINT64, GetDouble, GetGUID
    12, 20, 16,          // GetStringLength, GetString, GetAllocatedString
    12, 20, 16,          // GetBlobSize, GetBlob, GetAllocatedBlob
    16,                  // GetUnknown
    12, 8, 4,            // SetItem, DeleteItem, DeleteAllItems
    12, 16, 16, 12, 12,  // SetUINT32, SetUINT64, SetDouble, SetGUID, SetString
    16, 12,              // SetBlob, SetUnknown
    4, 4, 8, 16, 8,      // LockStore, UnlockStore, GetCount, GetItemByIndex, CopyAllItems
    8, 8, 12, 24, 24,    // GetMajorType, IsCompressedFormat, IsEqual,
                         // GetRepresentation(GUID by value), FreeRepresentation
}};

[[nodiscard]] bool is_attribute_type(u16 vt) {
    return vt == VT_UI4_T || vt == VT_UI8_T || vt == VT_R8_T ||
           vt == VT_CLSID_T || vt == VT_LPWSTR_T || vt == VT_BLOB_T ||
           vt == VT_UNKNOWN_T;
}

/// Media type attributes IsEqual's FORMAT_DATA comparison skips (Learn).
[[nodiscard]] bool excluded_from_format_data(const MfGuid& key) {
    return key == MF_MT_USER_DATA || key == MF_MT_FRAME_RATE_RANGE_MIN ||
           key == MF_MT_FRAME_RATE_RANGE_MAX;
}

/// Read a NUL-terminated UTF-16 string as its code units (no transcoding:
/// the store hands back exactly what was set).
[[nodiscard]] bool read_utf16(Marshaler& m, GuestAddr addr, std::u16string& out) {
    out.clear();
    if (addr == 0) return false;
    for (usize i = 0; i < 0x10000; ++i) {
        auto unit = m.read_u16(addr + i * 2);
        if (unit.is_err()) return false;
        if (unit.value() == 0) return true;
        out.push_back(static_cast<char16_t>(unit.value()));
    }
    return false;
}

void unimplemented_once(std::atomic<bool>& flag, const char* api, const char* why) {
    if (flag.exchange(true)) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED mfplat!%s: %s\n", api, why);
}

} // namespace

// ---------------------------------------------------------------------------
// Platform state
// ---------------------------------------------------------------------------

u32 MfPlatManager::startup(u32 version) {
    if ((version >> 16) > MF_SDK_VERSION) return MF_E_BAD_STARTUP_VERSION_HR;
    m_startups.fetch_add(1);
    return S_OK_HR;
}

u32 MfPlatManager::shutdown() {
    u32 expected = m_startups.load();
    while (expected != 0 &&
           !m_startups.compare_exchange_weak(expected, expected - 1)) {
    }
    return S_OK_HR;
}

u32 MfPlatManager::lock_platform() {
    m_locks.fetch_add(1);
    return S_OK_HR;
}

u32 MfPlatManager::unlock_platform() {
    u32 expected = m_locks.load();
    while (expected != 0 &&
           !m_locks.compare_exchange_weak(expected, expected - 1)) {
    }
    return S_OK_HR;
}

// ---------------------------------------------------------------------------
// Slot tables
// ---------------------------------------------------------------------------

u32 MfPlatManager::slot_count(MfIface iface) {
    return iface == MfIface::MediaType ? kMediaTypeSlots : kAttributesSlots;
}

u32 MfPlatManager::slot_stdcall_bytes(MfIface iface, u32 slot) {
    return slot < slot_count(iface) ? kStdcallBytes[slot] : 0u;
}

std::string MfPlatManager::slot_thunk_name(MfIface iface, u32 slot) {
    return std::string("Vitreo_") +
           (iface == MfIface::MediaType ? "IMFMediaType" : "IMFAttributes") +
           "_" + std::to_string(slot);
}

// ---------------------------------------------------------------------------
// Guest memory: cells and vtables
// ---------------------------------------------------------------------------

GuestAddr MfPlatManager::build_vtable(Marshaler& m, MfIface iface) {
    const auto index = static_cast<usize>(iface);
    if (m_vtables[index] != 0) return m_vtables[index];
    if (m_thunks == nullptr || m_next_addr == 0) return 0;

    const u32 count = slot_count(iface);
    const GuestAddr ptr_size = m.cpu_is_64bit() ? 8u : 4u;
    if (m_next_addr + ptr_size * count > m_scratch_base + MFPLAT_SCRATCH_SIZE) return 0;
    const GuestAddr vtable = m_next_addr;
    for (u32 slot = 0; slot < count; ++slot) {
        auto addr = m_thunks->get_thunk_address("mfplat.dll", slot_thunk_name(iface, slot));
        if (addr.is_err() || addr.value() == 0) return 0;
        if (m.write_ptr(vtable + static_cast<GuestAddr>(slot) * ptr_size, addr.value()).is_err()) {
            return 0;
        }
    }
    m_next_addr += ptr_size * count;
    m_vtables[index] = vtable;
    return vtable;
}

GuestAddr MfPlatManager::alloc_cell(Marshaler& m) {
    (void)m;
    constexpr GuestAddr kCell = 16;
    if (!m_free_cells.empty()) {
        const GuestAddr cell = m_free_cells.back();
        m_free_cells.pop_back();
        return cell;
    }
    if (m_next_addr == 0 || m_next_addr + kCell > m_scratch_base + MFPLAT_SCRATCH_SIZE) return 0;
    const GuestAddr cell = m_next_addr;
    m_next_addr += kCell;
    return cell;
}

void MfPlatManager::free_cell(GuestAddr cell) { m_free_cells.push_back(cell); }

u32 MfPlatManager::create_object(Marshaler& m, MfIface iface, GuestAddr out) {
    if (out == 0) return E_POINTER_HR;
    std::lock_guard<std::mutex> lock(m_mutex);
    const GuestAddr vtable = build_vtable(m, iface);
    if (vtable == 0) {
        (void)m.write_ptr(out, 0);
        return E_OUTOFMEMORY_HR;
    }
    const GuestAddr cell = alloc_cell(m);
    if (cell == 0 || m.write_ptr(cell, vtable).is_err()) {
        if (cell != 0) free_cell(cell);
        (void)m.write_ptr(out, 0);
        return E_OUTOFMEMORY_HR;
    }
    auto st = std::make_shared<Store>();
    st->iface = iface;
    m_objects[cell] = st;
    if (m.write_ptr(out, cell).is_err()) {
        m_objects.erase(cell);
        free_cell(cell);
        return E_POINTER_HR;
    }
    return S_OK_HR;
}

MfPlatManager::StorePtr MfPlatManager::find(GuestAddr self) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_objects.find(self);
    return it == m_objects.end() ? nullptr : it->second;
}

usize MfPlatManager::live_object_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_objects.size();
}

// ---------------------------------------------------------------------------
// Guest COM helpers
// ---------------------------------------------------------------------------

void MfPlatManager::guest_add_ref(Marshaler& m, GuestAddr punk) {
    if (punk == 0 || !m_guest_call) return;
    const u64 args[1] = {punk};
    (void)m_guest_call(m, punk, 1, args, 1);
}

void MfPlatManager::guest_release(Marshaler& m, GuestAddr punk) {
    if (punk == 0 || !m_guest_call) return;
    const u64 args[1] = {punk};
    (void)m_guest_call(m, punk, 2, args, 1);
}

bool MfPlatManager::read_guid(Marshaler& m, GuestAddr addr, MfGuid& out) const {
    if (addr == 0) return false;
    auto r = m.read_struct<MfGuid>(addr);
    if (r.is_err()) return false;
    out = r.value();
    return true;
}

bool MfPlatManager::write_guid(Marshaler& m, GuestAddr addr, const MfGuid& g) const {
    return addr != 0 && m.write_struct<MfGuid>(addr, g).is_ok();
}

// PROPVARIANT (propidl.h): vt at +0, the union at +8. On x64 the struct is
// 24 bytes and pointers are 8 wide; on x86 it is 16 bytes. BLOB/CAUB:
// cElems at +8, pElems at +8+pointer size.
bool MfPlatManager::read_propvariant(Marshaler& m, GuestAddr pv, Item& out) {
    if (pv == 0) return false;
    auto vt = m.read_u16(pv);
    if (vt.is_err()) return false;
    const GuestAddr ptr_size = m.cpu_is_64bit() ? 8u : 4u;
    out.vt = vt.value();
    switch (out.vt) {
        case VT_UI4_T: {
            auto v = m.read_u32(pv + 8);
            if (v.is_err()) return false;
            out.u32v = v.value();
            return true;
        }
        case VT_UI8_T: {
            auto v = m.read_u64(pv + 8);
            if (v.is_err()) return false;
            out.u64v = v.value();
            return true;
        }
        case VT_R8_T: {
            auto v = m.read_u64(pv + 8);
            if (v.is_err()) return false;
            u64 bits = v.value();
            std::memcpy(&out.dbl, &bits, sizeof(out.dbl));
            return true;
        }
        case VT_CLSID_T: {
            auto p = m.read_ptr(pv + 8);
            if (p.is_err()) return false;
            return read_guid(m, static_cast<GuestAddr>(p.value()), out.guid);
        }
        case VT_LPWSTR_T: {
            auto p = m.read_ptr(pv + 8);
            if (p.is_err() || p.value() == 0) return false;
            return read_utf16(m, static_cast<GuestAddr>(p.value()), out.str);
        }
        case VT_BLOB_T: {
            auto n = m.read_u32(pv + 8);
            auto p = m.read_ptr(pv + 8 + ptr_size);
            if (n.is_err() || p.is_err()) return false;
            out.blob.assign(n.value(), 0);
            if (n.value() != 0) {
                if (p.value() == 0) return false;
                if (m.memory().read_bytes(static_cast<GuestAddr>(p.value()),
                                          out.blob.data(), n.value()).is_err()) {
                    return false;
                }
            }
            return true;
        }
        case VT_UNKNOWN_T: {
            auto p = m.read_ptr(pv + 8);
            if (p.is_err()) return false;
            out.punk = static_cast<GuestAddr>(p.value());
            return true;
        }
        default:
            return true; // type carried for the caller's gate
    }
}

u32 MfPlatManager::write_propvariant(Marshaler& m, GuestAddr pv, const Item& item) {
    if (pv == 0) return E_POINTER_HR;
    const bool is64 = m.cpu_is_64bit();
    const GuestAddr ptr_size = is64 ? 8u : 4u;
    const u8 zero[24] = {};
    if (m.memory().write_bytes(pv, zero, is64 ? 24u : 16u).is_err()) return E_POINTER_HR;
    if (m.write_u16(pv, item.vt).is_err()) return E_POINTER_HR;
    switch (item.vt) {
        case VT_EMPTY_T:
            return S_OK_HR;
        case VT_UI4_T:
            return m.write_u32(pv + 8, item.u32v).is_ok() ? S_OK_HR : E_POINTER_HR;
        case VT_UI8_T:
            return m.write_u64(pv + 8, item.u64v).is_ok() ? S_OK_HR : E_POINTER_HR;
        case VT_R8_T: {
            u64 bits = 0;
            std::memcpy(&bits, &item.dbl, sizeof(bits));
            return m.write_u64(pv + 8, bits).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case VT_CLSID_T: {
            if (!m_task_alloc) return E_OUTOFMEMORY_HR;
            const GuestAddr p = m_task_alloc(16);
            if (p == 0) return E_OUTOFMEMORY_HR;
            if (!write_guid(m, p, item.guid) || m.write_ptr(pv + 8, p).is_err()) {
                m_task_free(p);
                return E_POINTER_HR;
            }
            return S_OK_HR;
        }
        case VT_LPWSTR_T: {
            if (!m_task_alloc) return E_OUTOFMEMORY_HR;
            const u64 bytes = (static_cast<u64>(item.str.size()) + 1) * 2;
            const GuestAddr p = m_task_alloc(bytes);
            if (p == 0) return E_OUTOFMEMORY_HR;
            std::vector<u8> raw(bytes, 0);
            for (usize i = 0; i < item.str.size(); ++i) {
                raw[i * 2] = static_cast<u8>(item.str[i] & 0xFFu);
                raw[i * 2 + 1] = static_cast<u8>(item.str[i] >> 8);
            }
            if (m.memory().write_bytes(p, raw.data(), raw.size()).is_err() ||
                m.write_ptr(pv + 8, p).is_err()) {
                m_task_free(p);
                return E_POINTER_HR;
            }
            return S_OK_HR;
        }
        case VT_BLOB_T: {
            GuestAddr p = 0;
            if (!item.blob.empty()) {
                if (!m_task_alloc) return E_OUTOFMEMORY_HR;
                p = m_task_alloc(item.blob.size());
                if (p == 0) return E_OUTOFMEMORY_HR;
                if (m.memory().write_bytes(p, item.blob.data(), item.blob.size()).is_err()) {
                    m_task_free(p);
                    return E_POINTER_HR;
                }
            }
            if (m.write_u32(pv + 8, static_cast<u32>(item.blob.size())).is_err() ||
                m.write_ptr(pv + 8 + ptr_size, p).is_err()) {
                if (p != 0) m_task_free(p);
                return E_POINTER_HR;
            }
            return S_OK_HR;
        }
        case VT_UNKNOWN_T:
            if (m.write_ptr(pv + 8, item.punk).is_err()) return E_POINTER_HR;
            guest_add_ref(m, item.punk);
            return S_OK_HR;
        default:
            return MF_E_INVALIDTYPE_HR;
    }
}

MfPlatManager::Item* MfPlatManager::find_item(Store& st, const MfGuid& key) {
    for (auto& it : st.items) {
        if (it.key == key) return &it;
    }
    return nullptr;
}

void MfPlatManager::put_item(Marshaler& m, Store& st, Item item) {
    GuestAddr release_after = 0;
    if (Item* existing = find_item(st, item.key)) {
        if (existing->vt == VT_UNKNOWN_T) release_after = existing->punk;
        *existing = std::move(item);
    } else {
        st.items.push_back(std::move(item));
    }
    // After the container is settled: Release may re-enter this store.
    if (release_after != 0) guest_release(m, release_after);
}

bool MfPlatManager::items_equal(const Item& a, const Item& b) {
    if (a.vt != b.vt) return false;
    switch (a.vt) {
        case VT_EMPTY_T: return true;
        case VT_UI4_T: return a.u32v == b.u32v;
        case VT_UI8_T: return a.u64v == b.u64v;
        case VT_R8_T: return a.dbl == b.dbl;
        case VT_CLSID_T: return a.guid == b.guid;
        case VT_LPWSTR_T: return a.str == b.str;
        case VT_BLOB_T: return a.blob == b.blob;
        case VT_UNKNOWN_T: return a.punk == b.punk;
        default: return false;
    }
}

bool MfPlatManager::contains_equal(const Store& st, const Item& it) {
    for (const auto& other : st.items) {
        if (other.key == it.key) return items_equal(other, it);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

u32 MfPlatManager::dispatch(MfIface iface, u32 slot, Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    StorePtr st = find(self);
    if (!st) return E_POINTER_HR; // not an object this component handed out
    std::unique_lock<std::recursive_mutex> lock(st->mutex);
    if (slot < 3) return dispatch_unknown(slot, m, self, *st);
    if (slot < kAttributesSlots) return dispatch_attributes(slot, m, self, *st);
    if (iface == MfIface::MediaType && slot < kMediaTypeSlots) {
        return dispatch_media_type(slot, m, self, *st);
    }
    return E_NOTIMPL_HR;
}

u32 MfPlatManager::dispatch_unknown(u32 slot, Marshaler& m, GuestAddr self, Store& st) {
    switch (slot) {
        case 0: { // QueryInterface(REFIID, void**)
            const GuestAddr ppv = m.arg_ptr(2);
            MfGuid iid;
            if (ppv == 0) return E_POINTER_HR;
            if (!read_guid(m, m.arg_ptr(1), iid)) return E_INVALIDARG_HR;
            const bool ok = iid == from_bytes(IID_IUNKNOWN) ||
                            iid == from_bytes(IID_IMFATTRIBUTES) ||
                            (st.iface == MfIface::MediaType &&
                             iid == from_bytes(IID_IMFMEDIATYPE));
            if (!ok) {
                (void)m.write_ptr(ppv, 0);
                return E_NOINTERFACE_HR;
            }
            if (m.write_ptr(ppv, self).is_err()) return E_POINTER_HR;
            ++st.refs;
            return S_OK_HR;
        }
        case 1: // AddRef
            return ++st.refs;
        case 2: { // Release
            if (st.refs > 1) return --st.refs;
            st.refs = 0;
            std::vector<GuestAddr> unknowns;
            for (const auto& it : st.items) {
                if (it.vt == VT_UNKNOWN_T && it.punk != 0) unknowns.push_back(it.punk);
            }
            st.items.clear();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_objects.erase(self);
                free_cell(self);
            }
            for (const GuestAddr punk : unknowns) guest_release(m, punk);
            return 0;
        }
        default:
            return E_NOTIMPL_HR;
    }
}

u32 MfPlatManager::dispatch_attributes(u32 slot, Marshaler& m, GuestAddr self, Store& st) {
    const bool is64 = m.cpu_is_64bit();
    MfGuid key;
    // Every slot but Compare, DeleteAllItems, LockStore, UnlockStore,
    // GetCount, GetItemByIndex and CopyAllItems starts with REFGUID guidKey.
    const bool has_key = !(slot == 6 || slot == 20 || slot == 28 || slot == 29 ||
                           slot == 30 || slot == 31 || slot == 32);
    if (has_key && !read_guid(m, m.arg_ptr(1), key)) return E_INVALIDARG_HR;
    Item* item = has_key ? find_item(st, key) : nullptr;

    // Typed getter: MF_E_ATTRIBUTENOTFOUND, then MF_E_INVALIDTYPE; the output
    // is untouched on either (measured).
    const auto typed = [&](u16 vt) -> u32 {
        if (item == nullptr) return MF_E_ATTRIBUTENOTFOUND_HR;
        if (item->vt != vt) return MF_E_INVALIDTYPE_HR;
        return S_OK_HR;
    };

    switch (slot) {
        case 3: { // GetItem(key, PROPVARIANT* pValue)
            if (item == nullptr) return MF_E_ATTRIBUTENOTFOUND_HR;
            const GuestAddr pv = m.arg_ptr(2);
            if (pv == 0) return S_OK_HR; // Learn: a null pValue only tests the key
            return write_propvariant(m, pv, *item);
        }
        case 4: { // GetItemType(key, MF_ATTRIBUTE_TYPE*)
            if (item == nullptr) return MF_E_ATTRIBUTENOTFOUND_HR;
            return m.write_u32(m.arg_ptr(2), item->vt).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case 5: { // CompareItem(key, REFPROPVARIANT, BOOL*)
            const GuestAddr pb = m.arg_ptr(3);
            Item theirs;
            bool equal = false;
            if (item != nullptr && read_propvariant(m, m.arg_ptr(2), theirs)) {
                equal = items_equal(*item, theirs);
            }
            return m.write_u32(pb, equal ? 1u : 0u).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case 6: { // Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE, BOOL*)
            const GuestAddr theirs_ptr = m.arg_ptr(1);
            const u32 match = m.arg32(2);
            const GuestAddr pb = m.arg_ptr(3);
            StorePtr theirs = find(theirs_ptr);
            if (!theirs) {
                static std::atomic<bool> once{false};
                unimplemented_once(once, "IMFAttributes::Compare",
                                   "pTheirs is not a Vitreo attribute store");
                return E_NOTIMPL_HR;
            }
            std::unique_lock<std::recursive_mutex> their_lock(theirs->mutex, std::defer_lock);
            if (theirs.get() != &st) their_lock.lock();
            const auto all_in = [](const Store& from, const Store& into) {
                for (const auto& it : from.items) {
                    if (!contains_equal(into, it)) return false;
                }
                return true;
            };
            bool result = false;
            switch (match) {
                case MF_ATTRIBUTES_MATCH_OUR_ITEMS: result = all_in(st, *theirs); break;
                case MF_ATTRIBUTES_MATCH_THEIR_ITEMS: result = all_in(*theirs, st); break;
                case MF_ATTRIBUTES_MATCH_ALL_ITEMS:
                    result = st.items.size() == theirs->items.size() && all_in(st, *theirs);
                    break;
                case MF_ATTRIBUTES_MATCH_INTERSECTION: {
                    result = true;
                    for (const auto& it : st.items) {
                        if (const Item* o = find_item(*theirs, it.key);
                            o != nullptr && !items_equal(*o, it)) {
                            result = false;
                            break;
                        }
                    }
                    break;
                }
                case MF_ATTRIBUTES_MATCH_SMALLER:
                    result = st.items.size() <= theirs->items.size()
                                 ? all_in(st, *theirs) : all_in(*theirs, st);
                    break;
                default:
                    return E_INVALIDARG_HR;
            }
            return m.write_u32(pb, result ? 1u : 0u).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case 7: { // GetUINT32
            if (const u32 hr = typed(VT_UI4_T); hr != S_OK_HR) return hr;
            return m.write_u32(m.arg_ptr(2), item->u32v).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case 8: { // GetUINT64
            if (const u32 hr = typed(VT_UI8_T); hr != S_OK_HR) return hr;
            return m.write_u64(m.arg_ptr(2), item->u64v).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case 9: { // GetDouble
            if (const u32 hr = typed(VT_R8_T); hr != S_OK_HR) return hr;
            u64 bits = 0;
            std::memcpy(&bits, &item->dbl, sizeof(bits));
            return m.write_u64(m.arg_ptr(2), bits).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case 10: { // GetGUID
            if (const u32 hr = typed(VT_CLSID_T); hr != S_OK_HR) return hr;
            return write_guid(m, m.arg_ptr(2), item->guid) ? S_OK_HR : E_POINTER_HR;
        }
        case 11: { // GetStringLength(key, UINT32*)
            if (const u32 hr = typed(VT_LPWSTR_T); hr != S_OK_HR) return hr;
            return m.write_u32(m.arg_ptr(2), static_cast<u32>(item->str.size())).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        }
        case 12: { // GetString(key, LPWSTR, UINT32 cchBufSize, UINT32* pcchLength)
            if (const u32 hr = typed(VT_LPWSTR_T); hr != S_OK_HR) return hr;
            const GuestAddr buf = m.arg_ptr(2);
            const u32 cch = m.arg32(3);
            const GuestAddr pcch = m.arg_ptr(4);
            const u32 len = static_cast<u32>(item->str.size());
            if (pcch != 0 && m.write_u32(pcch, len).is_err()) return E_POINTER_HR;
            // Measured: the buffer is untouched and the length is still
            // reported when the buffer cannot hold the string plus NUL.
            if (cch <= len) return E_NOT_SUFFICIENT_BUFFER_HR;
            std::vector<u8> raw((static_cast<usize>(len) + 1) * 2, 0);
            for (usize i = 0; i < len; ++i) {
                raw[i * 2] = static_cast<u8>(item->str[i] & 0xFFu);
                raw[i * 2 + 1] = static_cast<u8>(item->str[i] >> 8);
            }
            return m.memory().write_bytes(buf, raw.data(), raw.size()).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        }
        case 13: { // GetAllocatedString(key, LPWSTR*, UINT32*)
            if (const u32 hr = typed(VT_LPWSTR_T); hr != S_OK_HR) return hr;
            Item copy = *item;
            // Reuse the PROPVARIANT writer's string allocation through a
            // scratch PROPVARIANT-free path: allocate and copy directly.
            if (!m_task_alloc) return E_OUTOFMEMORY_HR;
            const u64 bytes = (static_cast<u64>(copy.str.size()) + 1) * 2;
            const GuestAddr p = m_task_alloc(bytes);
            if (p == 0) return E_OUTOFMEMORY_HR;
            std::vector<u8> raw(bytes, 0);
            for (usize i = 0; i < copy.str.size(); ++i) {
                raw[i * 2] = static_cast<u8>(copy.str[i] & 0xFFu);
                raw[i * 2 + 1] = static_cast<u8>(copy.str[i] >> 8);
            }
            if (m.memory().write_bytes(p, raw.data(), raw.size()).is_err() ||
                m.write_ptr(m.arg_ptr(2), p).is_err()) {
                m_task_free(p);
                return E_POINTER_HR;
            }
            if (const GuestAddr pcch = m.arg_ptr(3); pcch != 0) {
                (void)m.write_u32(pcch, static_cast<u32>(copy.str.size()));
            }
            return S_OK_HR;
        }
        case 14: { // GetBlobSize(key, UINT32*)
            if (const u32 hr = typed(VT_BLOB_T); hr != S_OK_HR) return hr;
            return m.write_u32(m.arg_ptr(2), static_cast<u32>(item->blob.size())).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        }
        case 15: { // GetBlob(key, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize)
            if (const u32 hr = typed(VT_BLOB_T); hr != S_OK_HR) return hr;
            const GuestAddr buf = m.arg_ptr(2);
            const u32 cb = m.arg32(3);
            const GuestAddr pcb = m.arg_ptr(4);
            const u32 size = static_cast<u32>(item->blob.size());
            if (pcb != 0 && m.write_u32(pcb, size).is_err()) return E_POINTER_HR;
            if (cb < size) return E_NOT_SUFFICIENT_BUFFER_HR;
            if (size == 0) return S_OK_HR;
            return m.memory().write_bytes(buf, item->blob.data(), size).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        }
        case 16: { // GetAllocatedBlob(key, UINT8**, UINT32*)
            if (const u32 hr = typed(VT_BLOB_T); hr != S_OK_HR) return hr;
            if (!m_task_alloc) return E_OUTOFMEMORY_HR;
            const u32 size = static_cast<u32>(item->blob.size());
            const GuestAddr p = m_task_alloc(size == 0 ? 1u : size);
            if (p == 0) return E_OUTOFMEMORY_HR;
            if ((size != 0 && m.memory().write_bytes(p, item->blob.data(), size).is_err()) ||
                m.write_ptr(m.arg_ptr(2), p).is_err()) {
                m_task_free(p);
                return E_POINTER_HR;
            }
            if (const GuestAddr pcb = m.arg_ptr(3); pcb != 0) (void)m.write_u32(pcb, size);
            return S_OK_HR;
        }
        case 17: { // GetUnknown(key, REFIID, LPVOID*)
            const GuestAddr ppv = m.arg_ptr(3);
            if (ppv == 0) return E_POINTER_HR;
            if (const u32 hr = typed(VT_UNKNOWN_T); hr != S_OK_HR) {
                (void)m.write_ptr(ppv, 0); // measured: null on not-found
                return hr;
            }
            if (!m_guest_call || item->punk == 0) {
                (void)m.write_ptr(ppv, 0);
                return E_NOINTERFACE_HR;
            }
            const u64 args[3] = {item->punk, m.arg_ptr(2), ppv};
            return m_guest_call(m, item->punk, 0, args, 3);
        }
        case 18: { // SetItem(key, REFPROPVARIANT)
            Item value;
            value.key = key;
            if (!read_propvariant(m, m.arg_ptr(2), value)) return E_INVALIDARG_HR;
            if (!is_attribute_type(value.vt)) {
                // Measured: the rejected key stays behind as an empty item.
                Item empty;
                empty.key = key;
                put_item(m, st, std::move(empty));
                return MF_E_INVALIDTYPE_HR;
            }
            if (value.vt == VT_UNKNOWN_T) guest_add_ref(m, value.punk);
            put_item(m, st, std::move(value));
            return S_OK_HR;
        }
        case 19: { // DeleteItem(key): S_OK whether or not it exists (measured)
            for (usize i = 0; i < st.items.size(); ++i) {
                if (st.items[i].key != key) continue;
                const GuestAddr punk = st.items[i].vt == VT_UNKNOWN_T ? st.items[i].punk : 0;
                st.items.erase(st.items.begin() + static_cast<std::ptrdiff_t>(i));
                guest_release(m, punk);
                break;
            }
            return S_OK_HR;
        }
        case 20: { // DeleteAllItems
            std::vector<GuestAddr> unknowns;
            for (const auto& it : st.items) {
                if (it.vt == VT_UNKNOWN_T && it.punk != 0) unknowns.push_back(it.punk);
            }
            st.items.clear();
            for (const GuestAddr punk : unknowns) guest_release(m, punk);
            return S_OK_HR;
        }
        case 21: { // SetUINT32(key, UINT32)
            Item v; v.key = key; v.vt = VT_UI4_T; v.u32v = m.arg32(2);
            put_item(m, st, std::move(v));
            return S_OK_HR;
        }
        case 22: { // SetUINT64(key, UINT64): two x86 stack slots
            Item v; v.key = key; v.vt = VT_UI8_T;
            v.u64v = is64 ? m.arg(2)
                          : ((m.arg(2) & 0xFFFFFFFFull) | (m.arg(3) << 32));
            put_item(m, st, std::move(v));
            return S_OK_HR;
        }
        case 23: { // SetDouble(key, double): XMM2 on x64, two slots on x86
            Item v; v.key = key; v.vt = VT_R8_T; v.dbl = m.arg_f64(2);
            put_item(m, st, std::move(v));
            return S_OK_HR;
        }
        case 24: { // SetGUID(key, REFGUID)
            Item v; v.key = key; v.vt = VT_CLSID_T;
            if (!read_guid(m, m.arg_ptr(2), v.guid)) return E_INVALIDARG_HR;
            put_item(m, st, std::move(v));
            return S_OK_HR;
        }
        case 25: { // SetString(key, LPCWSTR)
            const GuestAddr wsz = m.arg_ptr(2);
            if (wsz == 0) return E_INVALIDARG_HR; // not documented; unmeasured
            Item v; v.key = key; v.vt = VT_LPWSTR_T;
            if (!read_utf16(m, wsz, v.str)) return E_INVALIDARG_HR;
            put_item(m, st, std::move(v));
            return S_OK_HR;
        }
        case 26: { // SetBlob(key, const UINT8*, UINT32)
            const GuestAddr buf = m.arg_ptr(2);
            const u32 cb = m.arg32(3);
            Item v; v.key = key; v.vt = VT_BLOB_T; v.blob.assign(cb, 0);
            if (cb != 0 && (buf == 0 ||
                            m.memory().read_bytes(buf, v.blob.data(), cb).is_err())) {
                return E_INVALIDARG_HR;
            }
            put_item(m, st, std::move(v));
            return S_OK_HR;
        }
        case 27: { // SetUnknown(key, IUnknown*)
            const GuestAddr punk = m.arg_ptr(2);
            if (punk == 0) return E_INVALIDARG_HR; // not documented; unmeasured
            Item v; v.key = key; v.vt = VT_UNKNOWN_T; v.punk = punk;
            guest_add_ref(m, punk);
            put_item(m, st, std::move(v));
            return S_OK_HR;
        }
        case 28: // LockStore: held until UnlockStore from the same thread
            st.mutex.lock();
            ++st.lock_depth;
            return S_OK_HR;
        case 29: // UnlockStore
            if (st.lock_depth != 0) {
                --st.lock_depth;
                st.mutex.unlock();
            }
            return S_OK_HR;
        case 30: // GetCount(UINT32*)
            return m.write_u32(m.arg_ptr(1), static_cast<u32>(st.items.size())).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        case 31: { // GetItemByIndex(UINT32, GUID*, PROPVARIANT*)
            const u32 index = m.arg32(1);
            if (index >= st.items.size()) return E_INVALIDARG_HR; // outputs untouched
            const Item& it = st.items[index];
            if (const GuestAddr pkey = m.arg_ptr(2); pkey != 0 && !write_guid(m, pkey, it.key)) {
                return E_POINTER_HR;
            }
            if (const GuestAddr pv = m.arg_ptr(3); pv != 0) return write_propvariant(m, pv, it);
            return S_OK_HR;
        }
        case 32: { // CopyAllItems(IMFAttributes* pDest): replaces pDest's items
            const GuestAddr dest_ptr = m.arg_ptr(1);
            if (dest_ptr == 0) return E_POINTER_HR;
            StorePtr dest = find(dest_ptr);
            if (!dest) return copy_to_foreign(m, st, dest_ptr);
            if (dest.get() == &st) return S_OK_HR;
            std::unique_lock<std::recursive_mutex> dest_lock(dest->mutex);
            std::vector<GuestAddr> released;
            for (const auto& it : dest->items) {
                if (it.vt == VT_UNKNOWN_T && it.punk != 0) released.push_back(it.punk);
            }
            dest->items.clear();
            u32 hr = S_OK_HR;
            for (const auto& it : st.items) {
                if (it.vt == VT_EMPTY_T) { hr = MF_E_INVALIDTYPE_HR; break; }
                dest->items.push_back(it);
                if (it.vt == VT_UNKNOWN_T) guest_add_ref(m, it.punk);
            }
            for (const GuestAddr punk : released) guest_release(m, punk);
            return hr;
        }
        default:
            return E_NOTIMPL_HR;
    }
}

// CopyAllItems into an attribute store the guest implements: its
// DeleteAllItems (slot 20) and SetItem (slot 18) through its own vtable, with
// the key and PROPVARIANT staged in task memory for the duration of each call.
u32 MfPlatManager::copy_to_foreign(Marshaler& m, Store& src, GuestAddr dest) {
    if (!m_guest_call || !m_task_alloc) {
        static std::atomic<bool> once{false};
        unimplemented_once(once, "IMFAttributes::CopyAllItems",
                           "pDest is not a Vitreo attribute store and no guest "
                           "call path is configured");
        return E_NOTIMPL_HR;
    }
    const u64 clear_args[1] = {dest};
    if (const u32 hr = m_guest_call(m, dest, 20, clear_args, 1); hr & 0x80000000u) return hr;
    const bool is64 = m.cpu_is_64bit();
    const GuestAddr staging = m_task_alloc(16 + (is64 ? 24u : 16u));
    if (staging == 0) return E_OUTOFMEMORY_HR;
    u32 hr = S_OK_HR;
    for (const auto& it : src.items) {
        if (it.vt == VT_EMPTY_T) { hr = MF_E_INVALIDTYPE_HR; break; }
        const GuestAddr pv = staging + 16;
        if (!write_guid(m, staging, it.key)) { hr = E_POINTER_HR; break; }
        // write_propvariant AddRefs an IUnknown for the staged copy; the
        // guest's SetItem takes its own reference, so drop ours afterwards.
        if (hr = write_propvariant(m, pv, it); hr != S_OK_HR) break;
        const u64 args[3] = {dest, staging, pv};
        hr = m_guest_call(m, dest, 18, args, 3);
        // Free what the staged PROPVARIANT owns (PropVariantClear's job).
        if (it.vt == VT_CLSID_T || it.vt == VT_LPWSTR_T || it.vt == VT_BLOB_T) {
            auto p = m.read_ptr(pv + 8 + (it.vt == VT_BLOB_T ? (is64 ? 8u : 4u) : 0u));
            if (p.is_ok() && p.value() != 0) m_task_free(static_cast<GuestAddr>(p.value()));
        } else if (it.vt == VT_UNKNOWN_T) {
            guest_release(m, it.punk);
        }
        if (hr & 0x80000000u) break;
    }
    m_task_free(staging);
    return hr;
}

u32 MfPlatManager::dispatch_media_type(u32 slot, Marshaler& m, GuestAddr self, Store& st) {
    (void)self;
    const bool is64 = m.cpu_is_64bit();
    switch (slot) {
        case 33: { // GetMajorType(GUID*)
            const Item* major = find_item(st, MF_MT_MAJOR_TYPE);
            if (major == nullptr) return MF_E_ATTRIBUTENOTFOUND_HR;
            if (major->vt != VT_CLSID_T) return MF_E_INVALIDTYPE_HR;
            return write_guid(m, m.arg_ptr(1), major->guid) ? S_OK_HR : E_POINTER_HR;
        }
        case 34: { // IsCompressedFormat(BOOL*): FALSE iff MF_MT_ALL_SAMPLES_INDEPENDENT is TRUE
            const Item* independent = find_item(st, MF_MT_ALL_SAMPLES_INDEPENDENT);
            const bool compressed = !(independent != nullptr && independent->vt == VT_UI4_T &&
                                      independent->u32v != 0);
            return m.write_u32(m.arg_ptr(1), compressed ? 1u : 0u).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case 35: { // IsEqual(IMFMediaType*, DWORD* pdwFlags)
            const GuestAddr other_ptr = m.arg_ptr(1);
            const GuestAddr pflags = m.arg_ptr(2);
            if (other_ptr == 0 || pflags == 0) return E_POINTER_HR;
            StorePtr other = find(other_ptr);
            if (!other) {
                static std::atomic<bool> once{false};
                unimplemented_once(once, "IMFMediaType::IsEqual",
                                   "pIMediaType is not a Vitreo media type");
                return E_NOTIMPL_HR;
            }
            std::unique_lock<std::recursive_mutex> other_lock(other->mutex, std::defer_lock);
            if (other.get() != &st) other_lock.lock();
            const Item* ours_major = find_item(st, MF_MT_MAJOR_TYPE);
            const Item* their_major = find_item(*other, MF_MT_MAJOR_TYPE);
            if (ours_major == nullptr || their_major == nullptr) {
                (void)m.write_u32(pflags, 0); // measured: flags zeroed
                return E_INVALIDARG_HR;
            }
            u32 flags = 0;
            if (items_equal(*ours_major, *their_major)) flags |= MF_MEDIATYPE_EQUAL_MAJOR_TYPES;
            const Item* ours_sub = find_item(st, MF_MT_SUBTYPE);
            const Item* their_sub = find_item(*other, MF_MT_SUBTYPE);
            if ((ours_sub == nullptr && their_sub == nullptr) ||
                (ours_sub != nullptr && their_sub != nullptr && items_equal(*ours_sub, *their_sub))) {
                flags |= MF_MEDIATYPE_EQUAL_FORMAT_TYPES;
            }
            const Item* ours_ud = find_item(st, MF_MT_USER_DATA);
            const Item* their_ud = find_item(*other, MF_MT_USER_DATA);
            if ((ours_ud == nullptr && their_ud == nullptr) ||
                (ours_ud != nullptr && their_ud != nullptr && items_equal(*ours_ud, *their_ud))) {
                flags |= MF_MEDIATYPE_EQUAL_FORMAT_USER_DATA;
            }
            // FORMAT_DATA: the type with fewer (non-excluded) attributes is a
            // value-equal subset of the other.
            const auto counted = [](const Store& s) {
                usize n = 0;
                for (const auto& it : s.items) if (!excluded_from_format_data(it.key)) ++n;
                return n;
            };
            const Store& smaller = counted(st) <= counted(*other) ? st : *other;
            const Store& larger = (&smaller == &st) ? *other : st;
            bool subset = true;
            for (const auto& it : smaller.items) {
                if (excluded_from_format_data(it.key)) continue;
                if (!contains_equal(larger, it)) { subset = false; break; }
            }
            if (subset) flags |= MF_MEDIATYPE_EQUAL_FORMAT_DATA;
            if (m.write_u32(pflags, flags).is_err()) return E_POINTER_HR;
            return flags == (MF_MEDIATYPE_EQUAL_MAJOR_TYPES | MF_MEDIATYPE_EQUAL_FORMAT_TYPES |
                             MF_MEDIATYPE_EQUAL_FORMAT_DATA | MF_MEDIATYPE_EQUAL_FORMAT_USER_DATA)
                       ? S_OK_HR : S_FALSE_HR;
        }
        case 36: { // GetRepresentation(GUID guidRepresentation, LPVOID* ppv)
            // GUID by value: one pointer slot on x64, four stack slots on x86.
            // Measured: *ppv is left untouched on the failure.
            (void)m.arg_ptr(is64 ? 2 : 5);
            static std::atomic<bool> once{false};
            unimplemented_once(once, "IMFMediaType::GetRepresentation",
                               "AM_MEDIA_TYPE / FORMAT_* representations are not built");
            return MF_E_UNSUPPORTED_REPRESENTATION_HR;
        }
        case 37: { // FreeRepresentation(GUID, LPVOID): nothing was ever handed out
            static std::atomic<bool> once{false};
            unimplemented_once(once, "IMFMediaType::FreeRepresentation",
                               "no representation was produced by GetRepresentation");
            return MF_E_UNSUPPORTED_REPRESENTATION_HR;
        }
        default:
            return E_NOTIMPL_HR;
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_mf_object_thunks(DllExports& exports, MfPlatManager& mgr) {
    for (u32 i = 0; i < static_cast<u32>(MfIface::Count); ++i) {
        const auto iface = static_cast<MfIface>(i);
        const u32 count = MfPlatManager::slot_count(iface);
        for (u32 slot = 0; slot < count; ++slot) {
            exports.entries.push_back(ExportEntry{
                MfPlatManager::slot_thunk_name(iface, slot), 0,
                [&mgr, iface, slot](Marshaler& m) {
                    m.set_return(mgr.dispatch(iface, slot, m));
                },
                static_cast<u16>(MfPlatManager::slot_stdcall_bytes(iface, slot))});
        }
    }
}

} // namespace vitreo::winapi
