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

/// @file stgcreatedocfile_stgopenstorage_stgisstoragefile.cpp
/// @brief StgCreateDocfile / StgOpenStorage / StgIsStorageFile
///
/// Closed cut from storage.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// StgCreateDocfile / StgOpenStorage / StgIsStorageFile
// =============================================================================

u32 StorageManager::stg_create_docfile(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    const u32       mode     = static_cast<u32>(m.arg(1));
    const u32       reserved = static_cast<u32>(m.arg(2));
    const GuestAddr out_ptr  = m.arg_ptr(3);

    if (out_ptr == 0) return stg_error::E_INVALIDPOINTER;
    (void)m.write_ptr(out_ptr, 0);
    if (reserved != 0) return stg_error::E_INVALIDPARAMETER;
    if (m_vfs == nullptr) return stg_error::E_INSUFFICIENTMEMORY;

    // A NULL name asks for a temporary docfile, which Vitreo does not create.
    if (name_ptr == 0) {
        VITREO_TRACE("[ole32/stg] StgCreateDocfile(NULL): temporary docfiles "
                     "are not implemented -> STG_E_INVALIDPARAMETER\n");
        return stg_error::E_INVALIDPARAMETER;
    }
    auto name = m.read_wide_string(name_ptr);
    if (name.is_err() || name.value().empty()) {
        return stg_error::E_INVALIDNAME;
    }
    const std::string path = name.value();

    // Without STGM_CREATE an existing file is an error, as it is on Windows.
    const bool exists = m_vfs->stat(path).is_ok();
    if (exists && (mode & stgm::CREATE) == 0) {
        VITREO_TRACE("[ole32/stg] StgCreateDocfile(\"%s\"): exists and "
                     "STGM_CREATE not set -> STG_E_FILEALREADYEXISTS\n",
                     path.c_str());
        return stg_error::E_FILEALREADYEXISTS;
    }

    auto doc = std::make_shared<DocFile>();
    doc->path = path;
    doc->mode = mode;
    doc->root = std::make_shared<StorageNode>();
    doc->root->is_storage = true;
    doc->root->name = "Root Entry";

    // Write the (empty but valid) image immediately so the file exists with
    // the right shape even if the caller never commits.
    if (!flush(*doc)) return stg_error::E_WRITEFAULT;

    const GuestAddr object = create_storage_object(m, doc, doc->root, mode);
    if (object == 0) return stg_error::E_INSUFFICIENTMEMORY;
    if (m.write_ptr(out_ptr, object).is_err()) {
        return stg_error::E_INVALIDPOINTER;
    }
    VITREO_TRACE("[ole32/stg] StgCreateDocfile(\"%s\", mode=0x%x) -> "
                 "IStorage=0x%08x\n", path.c_str(), mode,
                 static_cast<unsigned>(object));
    return stg_error::S_OK;
}

u32 StorageManager::stg_open_storage(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    const u32       mode     = static_cast<u32>(m.arg(2));
    const GuestAddr exclude  = m.arg_ptr(3);
    const u32       reserved = static_cast<u32>(m.arg(4));
    const GuestAddr out_ptr  = m.arg_ptr(5);

    if (out_ptr == 0) return stg_error::E_INVALIDPOINTER;
    (void)m.write_ptr(out_ptr, 0);
    if (reserved != 0) return stg_error::E_INVALIDPARAMETER;
    if (name_ptr == 0) return stg_error::E_INVALIDNAME;
    if (exclude != 0) {
        // snbExclude asks for a partial instantiation; honouring it wrongly
        // would silently drop streams the caller expects to be absent.
        VITREO_TRACE("[ole32/stg] StgOpenStorage: snbExclude is not "
                     "supported -> STG_E_INVALIDPARAMETER\n");
        return stg_error::E_INVALIDPARAMETER;
    }
    if (m_vfs == nullptr) return stg_error::E_INSUFFICIENTMEMORY;

    auto name = m.read_wide_string(name_ptr);
    if (name.is_err() || name.value().empty()) return stg_error::E_INVALIDNAME;
    const std::string path = name.value();

    auto opened = m_vfs->open(path, capsule::AccessMode::Read,
                              capsule::ShareMode::Read,
                              capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) {
        VITREO_TRACE("[ole32/stg] StgOpenStorage(\"%s\"): not found\n",
                     path.c_str());
        return stg_error::E_FILENOTFOUND;
    }
    std::vector<u8> image;
    std::array<u8, 64 * 1024> chunk{};
    for (;;) {
        auto read = m_vfs->read(opened.value(), chunk);
        if (read.is_err() || read.value() == 0) break;
        image.insert(image.end(), chunk.begin(),
                     chunk.begin() + static_cast<std::ptrdiff_t>(read.value()));
    }
    (void)m_vfs->close(opened.value());

    auto root = parse_compound_file(
        std::span<const u8>(image.data(), image.size()));
    if (root == nullptr) {
        VITREO_TRACE("[ole32/stg] StgOpenStorage(\"%s\"): not a compound file "
                     "(%zu bytes) -> STG_E_INVALIDHEADER\n",
                     path.c_str(), image.size());
        return stg_error::E_INVALIDHEADER;
    }

    auto doc = std::make_shared<DocFile>();
    doc->path = path;
    doc->mode = mode;
    doc->root = std::move(root);

    const GuestAddr object = create_storage_object(m, doc, doc->root, mode);
    if (object == 0) return stg_error::E_INSUFFICIENTMEMORY;
    if (m.write_ptr(out_ptr, object).is_err()) {
        return stg_error::E_INVALIDPOINTER;
    }
    VITREO_TRACE("[ole32/stg] StgOpenStorage(\"%s\", mode=0x%x) -> "
                 "IStorage=0x%08x (%zu root elements)\n",
                 path.c_str(), mode, static_cast<unsigned>(object),
                 doc->root->children.size());
    return stg_error::S_OK;
}

u32 StorageManager::stg_is_storage_file(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    if (name_ptr == 0 || m_vfs == nullptr) return stg_error::E_INVALIDNAME;
    auto name = m.read_wide_string(name_ptr);
    if (name.is_err()) return stg_error::E_INVALIDNAME;

    auto opened = m_vfs->open(name.value(), capsule::AccessMode::Read,
                              capsule::ShareMode::Read,
                              capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return stg_error::E_FILENOTFOUND;
    std::array<u8, 8> header{};
    auto read = m_vfs->read(opened.value(), header);
    (void)m_vfs->close(opened.value());
    if (read.is_err() || read.value() < header.size()) return stg_error::S_FALSE;

    static constexpr std::array<u8, 8> kSignature{
        0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    return header == kSignature ? stg_error::S_OK : stg_error::S_FALSE;
}

} // namespace vitreo::winapi
