// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file storage.hpp
/// @brief OLE structured storage (compound files) — StgCreateDocfile & friends.
///
/// A compound file is a filesystem inside a file: named storages (directories)
/// containing named streams (files). Applications reach it through `IStorage`
/// and `IStream`, and the on-disk result has to be a real Compound File Binary
/// Format image, because the file outlives the process that wrote it — an
/// installer writes its uninstall log here and reads it back months later.
///
/// Vitreo models the tree in memory and serialises a valid CFBF image on
/// commit. What it does not do is pretend: an operation it cannot perform
/// returns the documented `STG_E_*` rather than a success the caller would
/// act on.

#include <vitreo/common.hpp>
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

class Marshaler;

/// STGM_* mode flags, as far as the storage layer distinguishes them.
namespace stgm {
constexpr u32 READ            = 0x00000000u;
constexpr u32 WRITE           = 0x00000001u;
constexpr u32 READWRITE       = 0x00000002u;
constexpr u32 ACCESS_MASK     = 0x00000003u;
constexpr u32 CREATE          = 0x00001000u;
constexpr u32 FAILIFTHERE     = 0x00000000u;
constexpr u32 TRANSACTED      = 0x00010000u;
constexpr u32 DELETEONRELEASE = 0x04000000u;
} // namespace stgm

/// STG_E_* / STG_S_* results used by this layer.
namespace stg_error {
constexpr u32 S_OK                 = 0x00000000u;
constexpr u32 S_FALSE              = 0x00000001u;
constexpr u32 E_INVALIDFUNCTION    = 0x80030001u;
constexpr u32 E_FILENOTFOUND       = 0x80030002u;
constexpr u32 E_PATHNOTFOUND       = 0x80030003u;
constexpr u32 E_ACCESSDENIED       = 0x80030005u;
constexpr u32 E_INSUFFICIENTMEMORY = 0x80030008u;
constexpr u32 E_INVALIDPOINTER     = 0x80030009u;
constexpr u32 E_WRITEFAULT         = 0x8003001Du;
constexpr u32 E_FILEALREADYEXISTS  = 0x80030050u;
constexpr u32 E_INVALIDPARAMETER   = 0x80030057u;
constexpr u32 E_INVALIDNAME        = 0x800300FCu;
constexpr u32 E_INVALIDHEADER      = 0x800300FBu;
constexpr u32 E_INVALIDFLAG        = 0x800300FFu;
} // namespace stg_error

/// One element of a compound file: a storage (with children) or a stream
/// (with bytes). Names compare case-insensitively, as they do on Windows.
struct StorageNode {
    std::string name;
    bool        is_storage{false};
    std::array<u8, 16> clsid{};
    std::vector<u8>    data;      ///< Stream contents; empty for a storage.
    /// [MS-CFB] 2.6.1 directory entry fields beyond the name and CLSID:
    /// State Bits, Creation Time and Modified Time (FILETIME). The reader
    /// fills them from disk and the writer emits them, so they survive a
    /// round trip; Authenticode's structured-storage digest covers them.
    u32 state_bits{0};
    u64 creation_time{0};
    u64 modified_time{0};
    /// The entry name exactly as the directory stores it (UTF-16 code units,
    /// no terminator). Filled by the reader; empty for nodes built in memory,
    /// whose `name` is authoritative. Digests and sort orders that are
    /// defined over the on-disk name use this.
    std::u16string name_utf16;

    /// Case-insensitive ordering so lookup matches Windows and so the
    /// serialised directory is deterministic.
    struct NameLess {
        [[nodiscard]] bool operator()(const std::string& a,
                                      const std::string& b) const noexcept;
    };
    std::map<std::string, std::shared_ptr<StorageNode>, NameLess> children;
};

/// An open compound file. The tree is shared by every IStorage/IStream object
/// handed out for it, so a write through one is visible through another —
/// which is what callers that hold two handles to the same docfile expect.
struct DocFile {
    std::string                  path;
    u32                          mode{0};
    std::shared_ptr<StorageNode> root;
    bool                         dirty{false};
};

/// Serialise a storage tree to a Compound File Binary Format image.
/// Exposed for testing: the writer is the part that has to be exactly right.
[[nodiscard]] std::vector<u8> serialize_compound_file(const StorageNode& root);

/// Parse a Compound File Binary Format image back into a tree.
/// Returns nullptr when the image is not a valid compound file.
[[nodiscard]] std::shared_ptr<StorageNode> parse_compound_file(
    std::span<const u8> image);

/// Serves the ole32 structured-storage entry points and the guest IStorage /
/// IStream objects they hand out.
class StorageManager {
public:
    StorageManager() = default;
    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;

    void set_vfs(capsule::Vfs* vfs) noexcept { m_vfs = vfs; }
    void set_thunk_registry(ThunkRegistry* thunks) noexcept { m_thunks = thunks; }

    /// StgCreateDocfile(pwcsName, grfMode, reserved, ppstgOpen)
    [[nodiscard]] u32 stg_create_docfile(Marshaler& m);

    /// StgOpenStorage(pwcsName, pstgPriority, grfMode, snbExclude, reserved,
    ///                ppstgOpen)
    [[nodiscard]] u32 stg_open_storage(Marshaler& m);

    /// StgIsStorageFile(pwcsName) — S_OK / S_FALSE / STG_E_FILENOTFOUND.
    [[nodiscard]] u32 stg_is_storage_file(Marshaler& m);

    // ----- IStorage vtable -----
    [[nodiscard]] u32 storage_query_interface(Marshaler& m);
    [[nodiscard]] u32 storage_add_ref(Marshaler& m);
    [[nodiscard]] u32 storage_release(Marshaler& m);
    [[nodiscard]] u32 storage_create_stream(Marshaler& m);
    [[nodiscard]] u32 storage_open_stream(Marshaler& m);
    [[nodiscard]] u32 storage_create_storage(Marshaler& m);
    [[nodiscard]] u32 storage_open_storage(Marshaler& m);
    [[nodiscard]] u32 storage_copy_to(Marshaler& m);
    [[nodiscard]] u32 storage_move_element_to(Marshaler& m);
    [[nodiscard]] u32 storage_commit(Marshaler& m);
    [[nodiscard]] u32 storage_revert(Marshaler& m);
    [[nodiscard]] u32 storage_enum_elements(Marshaler& m);
    [[nodiscard]] u32 storage_destroy_element(Marshaler& m);
    [[nodiscard]] u32 storage_rename_element(Marshaler& m);
    [[nodiscard]] u32 storage_set_element_times(Marshaler& m);
    [[nodiscard]] u32 storage_set_class(Marshaler& m);
    [[nodiscard]] u32 storage_set_state_bits(Marshaler& m);
    [[nodiscard]] u32 storage_stat(Marshaler& m);

    // ----- IStream vtable (streams inside a storage) -----
    [[nodiscard]] u32 stream_query_interface(Marshaler& m);
    [[nodiscard]] u32 stream_add_ref(Marshaler& m);
    [[nodiscard]] u32 stream_release(Marshaler& m);
    [[nodiscard]] u32 stream_read(Marshaler& m);
    [[nodiscard]] u32 stream_write(Marshaler& m);
    [[nodiscard]] u32 stream_seek(Marshaler& m);
    [[nodiscard]] u32 stream_set_size(Marshaler& m);
    [[nodiscard]] u32 stream_copy_to(Marshaler& m);
    [[nodiscard]] u32 stream_commit(Marshaler& m);
    [[nodiscard]] u32 stream_revert(Marshaler& m);
    [[nodiscard]] u32 stream_lock_region(Marshaler& m);
    [[nodiscard]] u32 stream_unlock_region(Marshaler& m);
    [[nodiscard]] u32 stream_stat(Marshaler& m);
    [[nodiscard]] u32 stream_clone(Marshaler& m);

private:
    struct StorageObject {
        std::shared_ptr<DocFile>     doc;
        std::shared_ptr<StorageNode> node;
        u32                          mode{0};
        u32                          ref_count{1};
    };
    struct StreamObject {
        std::shared_ptr<DocFile>     doc;
        std::shared_ptr<StorageNode> node;
        u64                          position{0};
        u32                          mode{0};
        u32                          ref_count{1};
    };

    [[nodiscard]] GuestAddr create_storage_object(
        Marshaler& m, std::shared_ptr<DocFile> doc,
        std::shared_ptr<StorageNode> node, u32 mode);
    [[nodiscard]] GuestAddr create_stream_object(
        Marshaler& m, std::shared_ptr<DocFile> doc,
        std::shared_ptr<StorageNode> node, u32 mode);

    /// Build a guest COM object: one vtable pointer followed by `names.size()`
    /// method slots resolved from the thunk registry.
    [[nodiscard]] GuestAddr build_com_object(
        Marshaler& m, std::span<const std::string_view> names);

    [[nodiscard]] std::shared_ptr<StorageObject> find_storage(GuestAddr object);
    [[nodiscard]] std::shared_ptr<StreamObject>  find_stream(GuestAddr object);

    /// Write the docfile's current tree out through the capsule VFS.
    [[nodiscard]] bool flush(DocFile& doc);

    capsule::Vfs*  m_vfs{nullptr};
    ThunkRegistry* m_thunks{nullptr};

    std::mutex m_mutex;
    std::unordered_map<GuestAddr, std::shared_ptr<StorageObject>> m_storages;
    std::unordered_map<GuestAddr, std::shared_ptr<StreamObject>>  m_streams;
    /// Bump allocator for the guest-visible object/vtable memory.
    GuestAddr m_scratch_next{0};
};

/// Guest address range backing IStorage/IStream objects.
///
/// Neighbours: gdi32's DIB scratch ends at 0x5ED00000 and shell32's PIDL
/// allocator starts at 0x5F000000. This 1 MB sits in the gap between them.
/// Overlapping shell32 is not a subtle failure — its allocator hands out PIDLs
/// from that base, so SHGetSpecialFolderLocation starts returning unusable
/// values and every shell path the guest builds silently becomes relative.
constexpr GuestAddr OLE_STORAGE_SCRATCH_BASE = 0x5ED00000;
constexpr u32       OLE_STORAGE_SCRATCH_SIZE = 0x100000; // 1 MB

/// Create the structured-storage export entries (merged into ole32.dll).
[[nodiscard]] DllExports create_storage_thunks(StorageManager& mgr);

} // namespace vitreo::winapi
