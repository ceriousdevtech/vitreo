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

/// @file object_construction.cpp
/// @brief Object construction
///
/// Closed cut from storage.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

namespace {

const std::array<std::string_view, 18> STORAGE_METHODS{{
    "Vitreo_Storage_QueryInterface", "Vitreo_Storage_AddRef",
    "Vitreo_Storage_Release",        "Vitreo_Storage_CreateStream",
    "Vitreo_Storage_OpenStream",     "Vitreo_Storage_CreateStorage",
    "Vitreo_Storage_OpenStorage",    "Vitreo_Storage_CopyTo",
    "Vitreo_Storage_MoveElementTo",  "Vitreo_Storage_Commit",
    "Vitreo_Storage_Revert",         "Vitreo_Storage_EnumElements",
    "Vitreo_Storage_DestroyElement", "Vitreo_Storage_RenameElement",
    "Vitreo_Storage_SetElementTimes","Vitreo_Storage_SetClass",
    "Vitreo_Storage_SetStateBits",   "Vitreo_Storage_Stat",
}};

const std::array<std::string_view, 14> STREAM_METHODS{{
    "Vitreo_StgStream_QueryInterface", "Vitreo_StgStream_AddRef",
    "Vitreo_StgStream_Release",        "Vitreo_StgStream_Read",
    "Vitreo_StgStream_Write",          "Vitreo_StgStream_Seek",
    "Vitreo_StgStream_SetSize",        "Vitreo_StgStream_CopyTo",
    "Vitreo_StgStream_Commit",         "Vitreo_StgStream_Revert",
    "Vitreo_StgStream_LockRegion",     "Vitreo_StgStream_UnlockRegion",
    "Vitreo_StgStream_Stat",           "Vitreo_StgStream_Clone",
}};

} // namespace

// =============================================================================
// Object construction
// =============================================================================

GuestAddr StorageManager::build_com_object(
    Marshaler& m, std::span<const std::string_view> names) {
    if (m_thunks == nullptr) return 0;

    std::vector<GuestAddr> methods(names.size());
    for (usize i = 0; i < names.size(); ++i) {
        auto thunk = m_thunks->get_thunk_address("ole32.dll", names[i]);
        if (thunk.is_err()) return 0;
        methods[i] = thunk.value();
    }

    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const u32 bytes = pointer_size * static_cast<u32>(1 + methods.size());
    if (m_scratch_next == 0) m_scratch_next = OLE_STORAGE_SCRATCH_BASE;
    if (m_scratch_next + bytes >
        OLE_STORAGE_SCRATCH_BASE + OLE_STORAGE_SCRATCH_SIZE) {
        return 0;
    }
    const GuestAddr object = m_scratch_next;
    m_scratch_next += (bytes + 15u) & ~15u;

    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return 0;
    for (usize i = 0; i < methods.size(); ++i) {
        if (m.write_ptr(vtable + static_cast<GuestAddr>(i) * pointer_size,
                        methods[i]).is_err()) {
            return 0;
        }
    }
    return object;
}

GuestAddr StorageManager::create_storage_object(
    Marshaler& m, std::shared_ptr<DocFile> doc,
    std::shared_ptr<StorageNode> node, u32 mode) {
    const GuestAddr object = build_com_object(m, STORAGE_METHODS);
    if (object == 0) return 0;
    auto state = std::make_shared<StorageObject>();
    state->doc = std::move(doc);
    state->node = std::move(node);
    state->mode = mode;
    std::lock_guard lock(m_mutex);
    m_storages[object] = std::move(state);
    return object;
}

GuestAddr StorageManager::create_stream_object(
    Marshaler& m, std::shared_ptr<DocFile> doc,
    std::shared_ptr<StorageNode> node, u32 mode) {
    const GuestAddr object = build_com_object(m, STREAM_METHODS);
    if (object == 0) return 0;
    auto state = std::make_shared<StreamObject>();
    state->doc = std::move(doc);
    state->node = std::move(node);
    state->mode = mode;
    std::lock_guard lock(m_mutex);
    m_streams[object] = std::move(state);
    return object;
}

std::shared_ptr<StorageManager::StorageObject>
StorageManager::find_storage(GuestAddr object) {
    std::lock_guard lock(m_mutex);
    auto it = m_storages.find(object);
    return it == m_storages.end() ? nullptr : it->second;
}

std::shared_ptr<StorageManager::StreamObject>
StorageManager::find_stream(GuestAddr object) {
    std::lock_guard lock(m_mutex);
    auto it = m_streams.find(object);
    return it == m_streams.end() ? nullptr : it->second;
}

bool StorageManager::flush(DocFile& doc) {
    if (m_vfs == nullptr || doc.root == nullptr) return false;

    const std::vector<u8> image = serialize_compound_file(*doc.root);
    auto opened = m_vfs->open(doc.path, capsule::AccessMode::Write,
                              capsule::ShareMode::None,
                              capsule::CreationDisposition::CreateAlways);
    if (opened.is_err()) {
        VITREO_TRACE("[ole32/stg] flush(\"%s\"): open failed\n",
                     doc.path.c_str());
        return false;
    }
    usize offset = 0;
    bool ok = true;
    while (offset < image.size()) {
        auto written = m_vfs->write(
            opened.value(),
            std::span<const u8>(image.data() + offset, image.size() - offset));
        if (written.is_err() || written.value() == 0) { ok = false; break; }
        offset += written.value();
    }
    (void)m_vfs->close(opened.value());
    if (ok) doc.dirty = false;
    VITREO_TRACE("[ole32/stg] flush(\"%s\") %zu bytes -> %s\n",
                 doc.path.c_str(), image.size(), ok ? "ok" : "FAILED");
    return ok;
}

} // namespace vitreo::winapi
