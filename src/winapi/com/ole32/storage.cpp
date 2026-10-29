// SPDX-License-Identifier: LGPL-2.1-only
/// @file storage.cpp
/// @brief IStorage / IStream objects over the compound-file model.
///
/// The tree lives in memory for the lifetime of the open docfile and is
/// serialised to a real CFBF image on commit (and on final release, which is
/// what direct-mode callers rely on — they never call Commit). Every object
/// handed to the guest is a vtable of thunk addresses plus host-side state,
/// the same shape as ole32's SHCreateMemStream stream.

#include "storage.hpp"

#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace vitreo::winapi {

namespace {

constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_INVALIDARG  = 0x80070057u;

constexpr u32 STGTY_STORAGE = 1;
constexpr u32 STGTY_STREAM  = 2;
constexpr u32 STATFLAG_NONAME = 1;

/// IID_IUnknown, IID_IStorage, IID_IStream, IID_ISequentialStream.
constexpr std::array<u8, 16> IID_IUNKNOWN{
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC0,0,0,0,0,0,0,0x46};
constexpr std::array<u8, 16> IID_ISTORAGE{
    0x0B,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC0,0,0,0,0,0,0,0x46};
constexpr std::array<u8, 16> IID_ISTREAM{
    0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC0,0,0,0,0,0,0,0x46};
constexpr std::array<u8, 16> IID_ISEQUENTIALSTREAM{
    0xC7,0x33,0xA3,0x0C,0x2A,0x1C,0x11,0xCE,0xAD,0xE5,0x00,0xAA,0x00,0x44,0x77,0x3D};

[[nodiscard]] bool iid_is(Marshaler& m, GuestAddr ptr,
                          const std::array<u8, 16>& expected) {
    std::array<u8, 16> actual{};
    if (m.memory().read_bytes(ptr, reinterpret_cast<Byte*>(actual.data()),
                              actual.size()).is_err()) {
        return false;
    }
    return actual == expected;
}

/// Element names are limited to 31 characters by the directory entry, and
/// may not contain the reserved separators.
[[nodiscard]] bool valid_element_name(const std::string& name) {
    if (name.empty() || name.size() > 31) return false;
    return name.find_first_of("\\/:!") == std::string::npos;
}

/// True when `mode` permits writing.
[[nodiscard]] bool mode_allows_write(u32 mode) {
    const u32 access = mode & stgm::ACCESS_MASK;
    return access == stgm::WRITE || access == stgm::READWRITE;
}

} // namespace



// =============================================================================
// IStorage
// =============================================================================

u32 StorageManager::storage_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid    = m.arg_ptr(1);
    const GuestAddr out    = m.arg_ptr(2);
    if (out == 0) return E_INVALIDARG;
    (void)m.write_ptr(out, 0);
    if (iid == 0) return E_INVALIDARG;

    auto state = find_storage(object);
    if (state == nullptr) return E_INVALIDARG;
    if (!iid_is(m, iid, IID_IUNKNOWN) && !iid_is(m, iid, IID_ISTORAGE)) {
        return E_NOINTERFACE;
    }
    if (m.write_ptr(out, object).is_err()) return E_INVALIDARG;
    std::lock_guard lock(m_mutex);
    ++state->ref_count;
    return stg_error::S_OK;
}

u32 StorageManager::storage_add_ref(Marshaler& m) {
    auto state = find_storage(m.arg_ptr(0));
    if (state == nullptr) return 0;
    std::lock_guard lock(m_mutex);
    return ++state->ref_count;
}

u32 StorageManager::storage_release(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    std::shared_ptr<DocFile> doc_to_flush;
    u32 remaining = 0;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_storages.find(object);
        if (it == m_storages.end()) return 0;
        remaining = --it->second->ref_count;
        if (remaining == 0) {
            // Direct-mode callers never call Commit, so the final release of
            // the root storage is the last chance to get the file on disk.
            if (it->second->doc != nullptr && it->second->doc->dirty &&
                it->second->node == it->second->doc->root) {
                doc_to_flush = it->second->doc;
            }
            m_storages.erase(it);
        }
    }
    if (doc_to_flush != nullptr) (void)flush(*doc_to_flush);
    return remaining;
}

u32 StorageManager::storage_create_stream(Marshaler& m) {
    const GuestAddr object   = m.arg_ptr(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    const u32       mode     = static_cast<u32>(m.arg(2));
    const u32       res1     = static_cast<u32>(m.arg(3));
    const u32       res2     = static_cast<u32>(m.arg(4));
    const GuestAddr out      = m.arg_ptr(5);

    if (out == 0) return stg_error::E_INVALIDPOINTER;
    (void)m.write_ptr(out, 0);
    if (res1 != 0 || res2 != 0) return stg_error::E_INVALIDPARAMETER;

    auto state = find_storage(object);
    if (state == nullptr || name_ptr == 0) return stg_error::E_INVALIDPARAMETER;
    if (!mode_allows_write(state->mode)) return stg_error::E_ACCESSDENIED;

    auto name = m.read_wide_string(name_ptr);
    if (name.is_err() || !valid_element_name(name.value())) {
        return stg_error::E_INVALIDNAME;
    }

    std::lock_guard lock(m_mutex);
    auto existing = state->node->children.find(name.value());
    if (existing != state->node->children.end()) {
        if ((mode & stgm::CREATE) == 0) return stg_error::E_FILEALREADYEXISTS;
        existing->second->data.clear();
        existing->second->is_storage = false;
    } else {
        auto child = std::make_shared<StorageNode>();
        child->name = name.value();
        state->node->children[name.value()] = child;
    }
    auto node = state->node->children[name.value()];
    state->doc->dirty = true;

    GuestAddr stream = 0;
    {
        // create_stream_object takes the same lock; build outside it.
        m_mutex.unlock();
        stream = create_stream_object(m, state->doc, node, mode);
        m_mutex.lock();
    }
    if (stream == 0) return stg_error::E_INSUFFICIENTMEMORY;
    if (m.write_ptr(out, stream).is_err()) return stg_error::E_INVALIDPOINTER;
    VITREO_TRACE("[ole32/stg] IStorage::CreateStream(\"%s\") -> 0x%08x\n",
                 name.value().c_str(), static_cast<unsigned>(stream));
    return stg_error::S_OK;
}

u32 StorageManager::storage_open_stream(Marshaler& m) {
    const GuestAddr object   = m.arg_ptr(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    const GuestAddr res1     = m.arg_ptr(2);
    const u32       mode     = static_cast<u32>(m.arg(3));
    const u32       res2     = static_cast<u32>(m.arg(4));
    const GuestAddr out      = m.arg_ptr(5);

    if (out == 0) return stg_error::E_INVALIDPOINTER;
    (void)m.write_ptr(out, 0);
    if (res1 != 0 || res2 != 0) return stg_error::E_INVALIDPARAMETER;

    auto state = find_storage(object);
    if (state == nullptr || name_ptr == 0) return stg_error::E_INVALIDPARAMETER;
    auto name = m.read_wide_string(name_ptr);
    if (name.is_err()) return stg_error::E_INVALIDNAME;

    std::shared_ptr<StorageNode> node;
    {
        std::lock_guard lock(m_mutex);
        auto it = state->node->children.find(name.value());
        if (it == state->node->children.end() || it->second->is_storage) {
            return stg_error::E_FILENOTFOUND;
        }
        node = it->second;
    }

    const GuestAddr stream = create_stream_object(m, state->doc, node, mode);
    if (stream == 0) return stg_error::E_INSUFFICIENTMEMORY;
    if (m.write_ptr(out, stream).is_err()) return stg_error::E_INVALIDPOINTER;
    VITREO_TRACE("[ole32/stg] IStorage::OpenStream(\"%s\") -> 0x%08x "
                 "(%zu bytes)\n", name.value().c_str(),
                 static_cast<unsigned>(stream), node->data.size());
    return stg_error::S_OK;
}

u32 StorageManager::storage_create_storage(Marshaler& m) {
    const GuestAddr object   = m.arg_ptr(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    const u32       mode     = static_cast<u32>(m.arg(2));
    const GuestAddr out      = m.arg_ptr(5);

    if (out == 0) return stg_error::E_INVALIDPOINTER;
    (void)m.write_ptr(out, 0);

    auto state = find_storage(object);
    if (state == nullptr || name_ptr == 0) return stg_error::E_INVALIDPARAMETER;
    if (!mode_allows_write(state->mode)) return stg_error::E_ACCESSDENIED;
    auto name = m.read_wide_string(name_ptr);
    if (name.is_err() || !valid_element_name(name.value())) {
        return stg_error::E_INVALIDNAME;
    }

    std::shared_ptr<StorageNode> node;
    {
        std::lock_guard lock(m_mutex);
        auto it = state->node->children.find(name.value());
        if (it != state->node->children.end()) {
            if ((mode & stgm::CREATE) == 0) {
                return stg_error::E_FILEALREADYEXISTS;
            }
            it->second->children.clear();
            it->second->is_storage = true;
            node = it->second;
        } else {
            node = std::make_shared<StorageNode>();
            node->name = name.value();
            node->is_storage = true;
            state->node->children[name.value()] = node;
        }
        state->doc->dirty = true;
    }

    const GuestAddr child = create_storage_object(m, state->doc, node, mode);
    if (child == 0) return stg_error::E_INSUFFICIENTMEMORY;
    if (m.write_ptr(out, child).is_err()) return stg_error::E_INVALIDPOINTER;
    VITREO_TRACE("[ole32/stg] IStorage::CreateStorage(\"%s\") -> 0x%08x\n",
                 name.value().c_str(), static_cast<unsigned>(child));
    return stg_error::S_OK;
}

u32 StorageManager::storage_open_storage(Marshaler& m) {
    const GuestAddr object   = m.arg_ptr(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    const u32       mode     = static_cast<u32>(m.arg(3));
    const GuestAddr exclude  = m.arg_ptr(4);
    const GuestAddr out      = m.arg_ptr(6);

    if (out == 0) return stg_error::E_INVALIDPOINTER;
    (void)m.write_ptr(out, 0);
    if (exclude != 0) return stg_error::E_INVALIDPARAMETER;

    auto state = find_storage(object);
    if (state == nullptr || name_ptr == 0) return stg_error::E_INVALIDPARAMETER;
    auto name = m.read_wide_string(name_ptr);
    if (name.is_err()) return stg_error::E_INVALIDNAME;

    std::shared_ptr<StorageNode> node;
    {
        std::lock_guard lock(m_mutex);
        auto it = state->node->children.find(name.value());
        if (it == state->node->children.end() || !it->second->is_storage) {
            return stg_error::E_FILENOTFOUND;
        }
        node = it->second;
    }
    const GuestAddr child = create_storage_object(m, state->doc, node, mode);
    if (child == 0) return stg_error::E_INSUFFICIENTMEMORY;
    if (m.write_ptr(out, child).is_err()) return stg_error::E_INVALIDPOINTER;
    return stg_error::S_OK;
}

u32 StorageManager::storage_commit(Marshaler& m) {
    auto state = find_storage(m.arg_ptr(0));
    if (state == nullptr) return stg_error::E_INVALIDPARAMETER;
    if (!mode_allows_write(state->mode)) return stg_error::E_ACCESSDENIED;
    return flush(*state->doc) ? stg_error::S_OK : stg_error::E_WRITEFAULT;
}

u32 StorageManager::storage_revert(Marshaler& m) {
    // Vitreo opens docfiles in direct mode: writes have already been applied
    // to the shared tree, so there is no transaction to roll back. Saying
    // S_OK would tell the caller its changes were discarded when they were
    // not.
    auto state = find_storage(m.arg_ptr(0));
    if (state == nullptr) return stg_error::E_INVALIDPARAMETER;
    if ((state->mode & stgm::TRANSACTED) == 0) return stg_error::S_OK;
    VITREO_TRACE("[ole32/stg] IStorage::Revert on a transacted storage is not "
                 "supported -> STG_E_INVALIDFUNCTION\n");
    return stg_error::E_INVALIDFUNCTION;
}

u32 StorageManager::storage_destroy_element(Marshaler& m) {
    const GuestAddr object   = m.arg_ptr(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    auto state = find_storage(object);
    if (state == nullptr || name_ptr == 0) return stg_error::E_INVALIDPARAMETER;
    if (!mode_allows_write(state->mode)) return stg_error::E_ACCESSDENIED;
    auto name = m.read_wide_string(name_ptr);
    if (name.is_err()) return stg_error::E_INVALIDNAME;

    std::lock_guard lock(m_mutex);
    if (state->node->children.erase(name.value()) == 0) {
        return stg_error::E_FILENOTFOUND;
    }
    state->doc->dirty = true;
    return stg_error::S_OK;
}

u32 StorageManager::storage_rename_element(Marshaler& m) {
    const GuestAddr object  = m.arg_ptr(0);
    const GuestAddr old_ptr = m.arg_ptr(1);
    const GuestAddr new_ptr = m.arg_ptr(2);
    auto state = find_storage(object);
    if (state == nullptr || old_ptr == 0 || new_ptr == 0) {
        return stg_error::E_INVALIDPARAMETER;
    }
    if (!mode_allows_write(state->mode)) return stg_error::E_ACCESSDENIED;
    auto old_name = m.read_wide_string(old_ptr);
    auto new_name = m.read_wide_string(new_ptr);
    if (old_name.is_err() || new_name.is_err() ||
        !valid_element_name(new_name.value())) {
        return stg_error::E_INVALIDNAME;
    }

    std::lock_guard lock(m_mutex);
    auto it = state->node->children.find(old_name.value());
    if (it == state->node->children.end()) return stg_error::E_FILENOTFOUND;
    if (state->node->children.count(new_name.value()) != 0) {
        return stg_error::E_FILEALREADYEXISTS;
    }
    auto node = it->second;
    state->node->children.erase(it);
    node->name = new_name.value();
    state->node->children[new_name.value()] = node;
    state->doc->dirty = true;
    return stg_error::S_OK;
}

u32 StorageManager::storage_set_class(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr clsid  = m.arg_ptr(1);
    auto state = find_storage(object);
    if (state == nullptr || clsid == 0) return stg_error::E_INVALIDPARAMETER;
    std::lock_guard lock(m_mutex);
    if (m.memory().read_bytes(clsid,
                              reinterpret_cast<Byte*>(state->node->clsid.data()),
                              state->node->clsid.size()).is_err()) {
        return stg_error::E_INVALIDPOINTER;
    }
    state->doc->dirty = true;
    return stg_error::S_OK;
}

u32 StorageManager::storage_stat(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr stat   = m.arg_ptr(1);
    const u32       flags  = static_cast<u32>(m.arg(2));
    auto state = find_storage(object);
    if (state == nullptr || stat == 0) return stg_error::E_INVALIDPARAMETER;

    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    std::vector<Byte> zero(pointer_size == 8 ? 80 : 72, 0);
    (void)m.memory().write_bytes(stat, zero.data(), zero.size());
    (void)m.write_ptr(stat, 0); // pwcsName: NULL unless a name was requested
    (void)m.write_u32(stat + pointer_size, STGTY_STORAGE);
    (void)m.memory().write_bytes(
        stat + pointer_size + 4 + 8 + 24 + 8,
        reinterpret_cast<const Byte*>(state->node->clsid.data()),
        state->node->clsid.size());
    if ((flags & STATFLAG_NONAME) == 0) {
        // The name must be CoTaskMemAlloc'd for the caller to free; Vitreo's
        // storage layer has no task allocator, so it reports that it cannot
        // supply one rather than handing back memory the caller would free
        // with the wrong allocator.
        VITREO_TRACE("[ole32/stg] IStorage::Stat with STATFLAG_DEFAULT (name "
                     "requested) is not supported -> STG_E_INVALIDFLAG\n");
        return stg_error::E_INVALIDFLAG;
    }
    return stg_error::S_OK;
}

u32 StorageManager::storage_copy_to(Marshaler& m) {
    (void)m.arg_ptr(0);
    VITREO_TRACE("[ole32/stg] IStorage::CopyTo is not implemented -> "
                 "STG_E_INVALIDFUNCTION\n");
    return stg_error::E_INVALIDFUNCTION;
}

u32 StorageManager::storage_move_element_to(Marshaler& m) {
    (void)m.arg_ptr(0);
    VITREO_TRACE("[ole32/stg] IStorage::MoveElementTo is not implemented -> "
                 "STG_E_INVALIDFUNCTION\n");
    return stg_error::E_INVALIDFUNCTION;
}

u32 StorageManager::storage_enum_elements(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(4);
    if (out != 0) (void)m.write_ptr(out, 0);
    VITREO_TRACE("[ole32/stg] IStorage::EnumElements needs an IEnumSTATSTG, "
                 "which is not implemented -> STG_E_INVALIDFUNCTION\n");
    return stg_error::E_INVALIDFUNCTION;
}

u32 StorageManager::storage_set_element_times(Marshaler& m) {
    // Vitreo does not track per-element times; the serialiser writes zeroes,
    // which is what a file created by a system without them looks like.
    (void)m.arg_ptr(0);
    return stg_error::S_OK;
}

u32 StorageManager::storage_set_state_bits(Marshaler& m) {
    (void)m.arg_ptr(0);
    return stg_error::S_OK;
}

// =============================================================================
// IStream over a storage element
// =============================================================================

u32 StorageManager::stream_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid    = m.arg_ptr(1);
    const GuestAddr out    = m.arg_ptr(2);
    if (out == 0) return E_INVALIDARG;
    (void)m.write_ptr(out, 0);
    if (iid == 0) return E_INVALIDARG;

    auto state = find_stream(object);
    if (state == nullptr) return E_INVALIDARG;
    if (!iid_is(m, iid, IID_IUNKNOWN) && !iid_is(m, iid, IID_ISTREAM) &&
        !iid_is(m, iid, IID_ISEQUENTIALSTREAM)) {
        return E_NOINTERFACE;
    }
    if (m.write_ptr(out, object).is_err()) return E_INVALIDARG;
    std::lock_guard lock(m_mutex);
    ++state->ref_count;
    return stg_error::S_OK;
}

u32 StorageManager::stream_add_ref(Marshaler& m) {
    auto state = find_stream(m.arg_ptr(0));
    if (state == nullptr) return 0;
    std::lock_guard lock(m_mutex);
    return ++state->ref_count;
}

u32 StorageManager::stream_release(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    std::lock_guard lock(m_mutex);
    auto it = m_streams.find(object);
    if (it == m_streams.end()) return 0;
    const u32 remaining = --it->second->ref_count;
    if (remaining == 0) m_streams.erase(it);
    return remaining;
}

u32 StorageManager::stream_read(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr buffer = m.arg_ptr(1);
    const u32       count  = static_cast<u32>(m.arg(2));
    const GuestAddr read_ptr = m.arg_ptr(3);

    auto state = find_stream(object);
    if (state == nullptr || buffer == 0) return stg_error::E_INVALIDPOINTER;

    std::lock_guard lock(m_mutex);
    const auto& data = state->node->data;
    const u64 available =
        state->position >= data.size() ? 0 : data.size() - state->position;
    const u32 to_read = static_cast<u32>(std::min<u64>(count, available));
    if (to_read != 0 &&
        m.memory().write_bytes(
            buffer, reinterpret_cast<const Byte*>(data.data() + state->position),
            to_read).is_err()) {
        return stg_error::E_INVALIDPOINTER;
    }
    state->position += to_read;
    if (read_ptr != 0) (void)m.write_u32(read_ptr, to_read);
    return to_read == count ? stg_error::S_OK : stg_error::S_FALSE;
}

u32 StorageManager::stream_write(Marshaler& m) {
    const GuestAddr object  = m.arg_ptr(0);
    const GuestAddr buffer  = m.arg_ptr(1);
    const u32       count   = static_cast<u32>(m.arg(2));
    const GuestAddr written_ptr = m.arg_ptr(3);

    auto state = find_stream(object);
    if (state == nullptr) return stg_error::E_INVALIDPOINTER;
    if (!mode_allows_write(state->mode)) return stg_error::E_ACCESSDENIED;
    if (buffer == 0 && count != 0) return stg_error::E_INVALIDPOINTER;

    std::lock_guard lock(m_mutex);
    auto& data = state->node->data;
    if (state->position + count > data.size()) {
        data.resize(static_cast<usize>(state->position + count), 0);
    }
    if (count != 0 &&
        m.memory().read_bytes(buffer,
                              reinterpret_cast<Byte*>(data.data() +
                                                      state->position),
                              count).is_err()) {
        return stg_error::E_INVALIDPOINTER;
    }
    state->position += count;
    state->doc->dirty = true;
    if (written_ptr != 0) (void)m.write_u32(written_ptr, count);
    return stg_error::S_OK;
}

u32 StorageManager::stream_seek(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const bool is_64bit = m.cpu_is_64bit();
    const u64 raw_move = is_64bit
        ? m.arg(1)
        : (static_cast<u64>(static_cast<u32>(m.arg(2))) << 32) |
              static_cast<u32>(m.arg(1));
    const i64 move   = static_cast<i64>(raw_move);
    const u32 origin = static_cast<u32>(m.arg(is_64bit ? 2 : 3));
    const GuestAddr out = m.arg_ptr(is_64bit ? 3 : 4);

    auto state = find_stream(object);
    if (state == nullptr || origin > 2) return stg_error::E_INVALIDFUNCTION;

    std::lock_guard lock(m_mutex);
    u64 base = 0;
    if (origin == 1)      base = state->position;
    else if (origin == 2) base = state->node->data.size();

    u64 next = 0;
    if (move < 0) {
        const u64 magnitude = static_cast<u64>(-(move + 1)) + 1;
        if (magnitude > base) return stg_error::E_INVALIDFUNCTION;
        next = base - magnitude;
    } else {
        next = base + static_cast<u64>(move);
    }
    state->position = next;
    if (out != 0 && m.write_u64(out, next).is_err()) {
        return stg_error::E_INVALIDPOINTER;
    }
    return stg_error::S_OK;
}

u32 StorageManager::stream_set_size(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const bool is_64bit = m.cpu_is_64bit();
    const u64 size = is_64bit
        ? m.arg(1)
        : (static_cast<u64>(static_cast<u32>(m.arg(2))) << 32) |
              static_cast<u32>(m.arg(1));

    auto state = find_stream(object);
    if (state == nullptr) return stg_error::E_INVALIDPOINTER;
    if (!mode_allows_write(state->mode)) return stg_error::E_ACCESSDENIED;
    if (size > (64u << 20)) return stg_error::E_INSUFFICIENTMEMORY;

    std::lock_guard lock(m_mutex);
    state->node->data.resize(static_cast<usize>(size), 0);
    state->doc->dirty = true;
    return stg_error::S_OK;
}

u32 StorageManager::stream_commit(Marshaler& m) {
    auto state = find_stream(m.arg_ptr(0));
    if (state == nullptr) return stg_error::E_INVALIDPOINTER;
    return flush(*state->doc) ? stg_error::S_OK : stg_error::E_WRITEFAULT;
}

u32 StorageManager::stream_revert(Marshaler& m) {
    (void)m.arg_ptr(0);
    return stg_error::S_OK; // Direct mode: nothing buffered to discard.
}

u32 StorageManager::stream_copy_to(Marshaler& m) {
    (void)m.arg_ptr(0);
    VITREO_TRACE("[ole32/stg] IStream::CopyTo is not implemented -> "
                 "STG_E_INVALIDFUNCTION\n");
    return stg_error::E_INVALIDFUNCTION;
}

u32 StorageManager::stream_lock_region(Marshaler& m) {
    // Windows compound files do not support byte-range locking either.
    (void)m.arg_ptr(0);
    return stg_error::E_INVALIDFUNCTION;
}

u32 StorageManager::stream_unlock_region(Marshaler& m) {
    (void)m.arg_ptr(0);
    return stg_error::E_INVALIDFUNCTION;
}

u32 StorageManager::stream_stat(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr stat   = m.arg_ptr(1);
    const u32       flags  = static_cast<u32>(m.arg(2));
    auto state = find_stream(object);
    if (state == nullptr || stat == 0) return stg_error::E_INVALIDPOINTER;

    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    std::vector<Byte> zero(pointer_size == 8 ? 80 : 72, 0);
    (void)m.memory().write_bytes(stat, zero.data(), zero.size());

    std::lock_guard lock(m_mutex);
    (void)m.write_ptr(stat, 0);
    (void)m.write_u32(stat + pointer_size, STGTY_STREAM);
    (void)m.write_u64(stat + pointer_size + 4,
                      static_cast<u64>(state->node->data.size()));
    if ((flags & STATFLAG_NONAME) == 0) {
        VITREO_TRACE("[ole32/stg] IStream::Stat with STATFLAG_DEFAULT (name "
                     "requested) is not supported -> STG_E_INVALIDFLAG\n");
        return stg_error::E_INVALIDFLAG;
    }
    return stg_error::S_OK;
}

u32 StorageManager::stream_clone(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out    = m.arg_ptr(1);
    if (out == 0) return stg_error::E_INVALIDPOINTER;
    (void)m.write_ptr(out, 0);

    auto state = find_stream(object);
    if (state == nullptr) return stg_error::E_INVALIDPOINTER;

    const GuestAddr clone =
        create_stream_object(m, state->doc, state->node, state->mode);
    if (clone == 0) return stg_error::E_INSUFFICIENTMEMORY;
    {
        std::lock_guard lock(m_mutex);
        m_streams[clone]->position = state->position;
    }
    if (m.write_ptr(out, clone).is_err()) return stg_error::E_INVALIDPOINTER;
    return stg_error::S_OK;
}

// =============================================================================
// Export table
// =============================================================================

DllExports create_storage_thunks(StorageManager& mgr) {
    DllExports exports;
    const auto add = [&exports](std::string_view name, u16 bytes,
                                ApiHandler handler) {
        exports.entries.push_back(
            ExportEntry{std::string(name), 0, std::move(handler), bytes});
    };

    add("StgCreateDocfile", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.stg_create_docfile(m)); });
    add("StgOpenStorage", 24,
        [&mgr](Marshaler& m) { m.set_return(mgr.stg_open_storage(m)); });
    add("StgIsStorageFile", 4,
        [&mgr](Marshaler& m) { m.set_return(mgr.stg_is_storage_file(m)); });

    // IStorage vtable slots.
    add("Vitreo_Storage_QueryInterface", 12,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_query_interface(m)); });
    add("Vitreo_Storage_AddRef", 4,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_add_ref(m)); });
    add("Vitreo_Storage_Release", 4,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_release(m)); });
    add("Vitreo_Storage_CreateStream", 24,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_create_stream(m)); });
    add("Vitreo_Storage_OpenStream", 24,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_open_stream(m)); });
    add("Vitreo_Storage_CreateStorage", 24,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_create_storage(m)); });
    add("Vitreo_Storage_OpenStorage", 28,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_open_storage(m)); });
    add("Vitreo_Storage_CopyTo", 20,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_copy_to(m)); });
    add("Vitreo_Storage_MoveElementTo", 20,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_move_element_to(m)); });
    add("Vitreo_Storage_Commit", 8,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_commit(m)); });
    add("Vitreo_Storage_Revert", 4,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_revert(m)); });
    add("Vitreo_Storage_EnumElements", 20,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_enum_elements(m)); });
    add("Vitreo_Storage_DestroyElement", 8,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_destroy_element(m)); });
    add("Vitreo_Storage_RenameElement", 12,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_rename_element(m)); });
    add("Vitreo_Storage_SetElementTimes", 20,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_set_element_times(m)); });
    add("Vitreo_Storage_SetClass", 8,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_set_class(m)); });
    add("Vitreo_Storage_SetStateBits", 12,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_set_state_bits(m)); });
    add("Vitreo_Storage_Stat", 12,
        [&mgr](Marshaler& m) { m.set_return(mgr.storage_stat(m)); });

    // IStream vtable slots.
    add("Vitreo_StgStream_QueryInterface", 12,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_query_interface(m)); });
    add("Vitreo_StgStream_AddRef", 4,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_add_ref(m)); });
    add("Vitreo_StgStream_Release", 4,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_release(m)); });
    add("Vitreo_StgStream_Read", 16,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_read(m)); });
    add("Vitreo_StgStream_Write", 16,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_write(m)); });
    add("Vitreo_StgStream_Seek", 20,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_seek(m)); });
    add("Vitreo_StgStream_SetSize", 12,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_set_size(m)); });
    add("Vitreo_StgStream_CopyTo", 24,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_copy_to(m)); });
    add("Vitreo_StgStream_Commit", 8,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_commit(m)); });
    add("Vitreo_StgStream_Revert", 4,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_revert(m)); });
    add("Vitreo_StgStream_LockRegion", 24,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_lock_region(m)); });
    add("Vitreo_StgStream_UnlockRegion", 24,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_unlock_region(m)); });
    add("Vitreo_StgStream_Stat", 12,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_stat(m)); });
    add("Vitreo_StgStream_Clone", 8,
        [&mgr](Marshaler& m) { m.set_return(mgr.stream_clone(m)); });

    return exports;
}

} // namespace vitreo::winapi
