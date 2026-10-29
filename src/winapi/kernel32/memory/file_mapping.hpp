#pragma once

/// @file file_mapping.hpp
/// @brief Memory-mapped files (WIN32-066)

#include <vitreo/common.hpp>
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/core/thunk_registry.hpp"

#include "ipc/shared_memory.hpp"

#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

/// Sentinel value matching Windows INVALID_HANDLE_VALUE for hFile
constexpr Handle FILE_MAPPING_INVALID_FILE = static_cast<Handle>(0xFFFFFFFF);

class FileManager;
class Marshaler;
class VirtualMemoryManager;

// =============================================================================
// Data Structures
// =============================================================================

/// Host-side representation of a file-mapping object
struct FileMappingObject {
    u64 size{0};
    u32 protection{0};
    std::string name;               ///< Empty for anonymous mappings
    std::vector<u8> backing_data;   ///< Eager private snapshot when mmap is unavailable
    std::string source_path;        ///< Guest file identity survives CloseHandle.
    Handle file_handle{FILE_MAPPING_INVALID_FILE}; ///< Source file handle (INVALID = anonymous)
    /// True when CreateFileMapping used PAGE_READWRITE (or execute-readwrite).
    /// Enables FILE_MAP_WRITE views and write-back to the underlying file.
    /// PAGE_WRITECOPY is not file-writable — COW pages never hit the file.
    bool writable{false};
    /// Cross-process backing (Stage 2). When set, views are mmap'd MAP_SHARED
    /// from this object so writes are visible across host processes; the
    /// per-process backing_data vector is unused.
    std::shared_ptr<ipc::SharedMemory> shared;
    /// Internal IPC name used to transfer an unnamed mapping to a child.
    /// This is deliberately separate from the Windows-visible object name.
    std::string transfer_name;
    /// Demand-paged host mapping of a file-backed section (Learn: creating a
    /// mapping object does not read the file; pages fault on view access).
    void* file_mmap{nullptr};
    usize file_mmap_len{0};
    /// CreateFileMapping SEC_RESERVE: MapView reserves VA; VirtualAlloc commits.
    bool sec_reserve{false};
    /// CreateFileMapping SEC_IMAGE / SEC_IMAGE_NO_EXECUTE: MapView is MEM_IMAGE.
    bool sec_image{false};
    /// SEC_IMAGE_NO_EXECUTE: all image pages stay PAGE_READONLY (Windows).
    bool sec_image_no_execute{false};
    /// Raw PE bytes for SEC_IMAGE views (file snapshot at create time).
    std::vector<u8> image_pe_bytes;
    /// SEC_IMAGE: the image's preferred ImageBase. A view placed elsewhere is
    /// STATUS_IMAGE_NOT_AT_BASE from NtMapViewOfSection.
    u64 image_base{0};

    FileMappingObject() = default;
    FileMappingObject(const FileMappingObject&) = delete;
    FileMappingObject& operator=(const FileMappingObject&) = delete;
    ~FileMappingObject();

    [[nodiscard]] const Byte* section_bytes() const noexcept;
    [[nodiscard]] Byte* section_bytes() noexcept;
    [[nodiscard]] usize section_byte_count() const noexcept;
};

/// Map-time contents of a FILE_MAP_COPY view. A page that differs from it was
/// privately written, so VirtualQuery reports its WRITECOPY protection as the
/// READWRITE form (writecopy_protect_probe map copy1 written q).
struct CopyViewTemplate {
    const Byte* data{nullptr};
    usize size{0};
    usize vm_size{0};  ///< Non-zero when `data` is a host VM copy-on-write remap.

    CopyViewTemplate() = default;
    CopyViewTemplate(const CopyViewTemplate&) = delete;
    CopyViewTemplate& operator=(const CopyViewTemplate&) = delete;
    ~CopyViewTemplate();
};

/// Tracks a single mapped view in guest memory
struct MappedView {
    GuestAddr base_addr{0};
    u64 offset{0};
    u64 size{0};
    usize guest_mapped_size{0};
    u32 desired_access{0};
    u32 allocation_protection{0};
    /// Exact current guest-visible protection runs, keyed by byte offset. Do
    /// not infer these from GuestMemory: PAGE_READWRITE and PAGE_WRITECOPY
    /// share the same native flags.
    std::map<u64, u32> protection_runs;
    std::shared_ptr<FileMappingObject> mapping;
    void* shared_host_ptr{nullptr}; ///< MAP_SHARED host mmap for shared views (munmap on unmap)
    usize shared_host_len{0};
    /// Non-zero when this view replaced a placeholder. Retained so an
    /// UnmapViewOfFile2 MEM_PRESERVE_PLACEHOLDER call can restore its origin.
    u64 placeholder_group{0};
    /// MapViewOfFile(FILE_MAP_COPY): private pages must not be written back
    /// to the section object or the file (Learn: lost on UnmapViewOfFile).
    bool copy_on_write{false};
    std::shared_ptr<const CopyViewTemplate> copy_template;
    /// Guest pages are remapped onto section host pages (file_mmap), so
    /// UnmapViewOfFile must not snapshot-copy and must not munmap the section.
    bool aliases_section{false};
    /// SEC_RESERVE view: pages with protection_runs value 0 are still reserved.
    bool sec_reserve{false};
    /// SEC_IMAGE view: VirtualQuery Type is MEM_IMAGE.
    bool sec_image{false};
};

enum class ViewDiscardResult {
    NotOwned,
    InvalidRange,
    Success,
    HostFailure,
};

enum class ViewProtectResult {
    NotOwned,
    InvalidRange,
    /// ERROR_INVALID_PARAMETER: the view's access cannot take this protection.
    InvalidProtection,
    /// Windows accepts the request but Vitreo cannot honour it (logged).
    Unimplemented,
    Success,
    HostFailure,
};

// =============================================================================
// FileMappingManager
// =============================================================================

class FileMappingManager {
public:
    FileMappingManager(HandleTable& handles, GuestMemory& memory,
                       u64 maximum_address = 0x7FFE'FFFFULL);

    /// Attach a FileManager for file-backed mapping support
    void set_file_manager(FileManager* fm) noexcept { m_file_mgr = fm; }

    /// A file handle FileManager does not own: NtCreateFile / NtOpenFile
    /// handles live in NtdllExtManager. Win32 and NT file handles share one
    /// namespace, so a section can be created over either.
    struct ForeignFile {
        u32 vfs{0xFFFFFFFFu};  ///< capsule::FileHandle (INVALID_FILE_HANDLE when unset)
        std::string path;
    };
    using ForeignFileResolver = std::function<std::optional<ForeignFile>(Handle)>;
    void set_foreign_file_resolver(ForeignFileResolver resolver) {
        m_foreign_files = std::move(resolver);
    }

    /// What NtCreateSection needs beyond CreateFileMapping's parameters.
    struct SectionCreateExtras {
        /// Rights the new handle is granted, in FILE_MAP_* bits
        /// (HandleAccess::All for CreateFileMapping's full-access handle).
        u32 granted_access{HandleAccess::All};
        /// Out: the NTSTATUS for a file SEC_IMAGE rejects
        /// (STATUS_INVALID_IMAGE_NOT_MZ / STATUS_INVALID_IMAGE_FORMAT);
        /// CreateFileMapping reports both as ERROR_BAD_EXE_FORMAT.
        u32 image_status{0};
    };

    /// CreateFileMappingW
    /// @param h_file INVALID_HANDLE (0xFFFFFFFF) for anonymous
    /// @param protect Page protection flags
    /// @param max_size Maximum size of the mapping
    /// @param name Optional mapping name (empty = anonymous)
    /// @return Valid handle on success, NULL_HANDLE on failure
    [[nodiscard]] Handle create_file_mapping(Handle h_file, u32 protect,
                                              u64 max_size, const std::string& name,
                                              bool inheritable = false,
                                              SectionCreateExtras* extras = nullptr);

    /// OpenFileMappingW — find an existing named mapping
    /// @param desired_access Desired access flags
    /// @param name Name to look up
    /// @return Handle or NULL_HANDLE if not found
    [[nodiscard]] Handle open_file_mapping(u32 desired_access,
                                           const std::string& name,
                                           bool inheritable = false);

    /// Duplicate an existing mapping handle. The new handle refers to the
    /// same mapping object but has an independent HandleTable lifetime.
    /// Duplicate a mapping handle with an independently granted access mask.
    [[nodiscard]] Handle duplicate_handle(Handle source, u32 flags,
                                          u32 access);

    /// Serialize inheritable mapping handles for a CreateProcess child.
    /// A null result means a selected section could not be made transferable.
    [[nodiscard]] std::optional<std::string> prepare_inherited_handles();
    [[nodiscard]] std::optional<std::string> prepare_inherited_handles(
        std::span<const Handle> selected_handles);

    /// Import inherited mapping handles at their parent-visible HANDLE values.
    [[nodiscard]] bool import_inherited_handles(std::string_view descriptors);

    /// Serialize one section for runtime DuplicateHandle into another guest
    /// process. The result excludes any process-local HANDLE value.
    [[nodiscard]] std::optional<std::string> prepare_transferred_handle(
        Handle source, u32 access, bool inheritable);
    /// Import a runtime-transferred section and allocate a new local HANDLE.
    [[nodiscard]] Handle import_transferred_handle(
        std::string_view descriptor);

    /// MapViewOfFile — map a view of a file-mapping into guest memory
    /// @param h_mapping Handle from create_file_mapping / open_file_mapping
    /// @param desired_access Desired access flags
    /// @param offset Offset into the mapping
    /// @param bytes_to_map Number of bytes to map (0 = entire mapping)
    /// @return Guest address of the mapped view, or 0 on failure
    [[nodiscard]] GuestAddr map_view_of_file(Handle h_mapping, u32 desired_access,
                                              u64 offset, u64 bytes_to_map);

    /// MapViewOfFileEx — map at the caller-specified base address. A non-zero
    /// address is exact (not a hint) and must be allocation-granularity
    /// aligned; a zero address has MapViewOfFile placement semantics.
    [[nodiscard]] GuestAddr map_view_of_file_ex(
        Handle h_mapping, u32 desired_access, u64 offset, u64 bytes_to_map,
        GuestAddr base_address);

    /// MapViewOfFile3 local-process behavior. Supports ordinary placement and
    /// exact MEM_REPLACE_PLACEHOLDER section views.
    [[nodiscard]] GuestAddr map_view_of_file3(
        Handle h_mapping, GuestAddr base_address, u64 offset,
        u64 view_size, u32 allocation_type, u32 page_protection,
        VirtualMemoryManager& virtual_memory);

    /// UnmapViewOfFile — unmap a previously mapped view
    /// @param base_address Address returned by map_view_of_file
    /// @return true on success
    bool unmap_view_of_file(GuestAddr base_address);

    /// UnmapViewOfFile2 local-process behavior, including restoration of a
    /// replaced placeholder when MEM_PRESERVE_PLACEHOLDER is requested.
    bool unmap_view_of_file2(GuestAddr base_address, u32 unmap_flags,
                             VirtualMemoryManager& virtual_memory);

    /// FlushViewOfFile — flush a mapped view to disk
    /// @param base_address Start of the range to flush
    /// @param num_bytes Number of bytes (0 = entire view)
    /// @return true on success
    bool flush_view_of_file(GuestAddr base_address, u64 num_bytes);

    /// If @p address falls inside a MapViewOfFile view, return that view's
    /// base and size so VirtualQuery can report MEM_MAPPED (Learn contract).
    [[nodiscard]] bool query_view(GuestAddr address, GuestAddr& view_base,
                                  u64& view_size, u32* protection = nullptr,
                                  u64* protection_run_end = nullptr,
                                  u32* allocation_protection = nullptr,
                                  bool* sec_image = nullptr,
                                  bool* page_reserved = nullptr) const noexcept;

    /// Learn SEC_RESERVE: VirtualAlloc MEM_COMMIT inside a reserved mapped view.
    [[nodiscard]] bool commit_reserved_view_pages(GuestAddr address, u64 size,
                                                  u32 protect);

    [[nodiscard]] std::string mapped_file_path(GuestAddr address) const;

    /// Discard a PAGE_READWRITE mapped-view range while preserving its view,
    /// protection, and backing ownership.
    [[nodiscard]] ViewDiscardResult discard_virtual_memory(
        GuestAddr address, u64 size);

    /// Change protection on a mapped-view subrange and retain exact Win32
    /// protection runs for VirtualQuery/DiscardVirtualMemory decisions.
    [[nodiscard]] ViewProtectResult virtual_protect_view(
        GuestAddr address, usize size, u32 new_protect, u32* old_protect);

    /// Dump candidate link-state slots (64-byte fragments in the 64-byte
    /// block region) from MAP_SHARED views so cross-process coherence can
    /// be checked.
    void dump_ipcz_router_link_state_candidates(const char* reason) const;

    /// Release this process handle's reference to a mapping object.
    bool on_handle_close(Handle handle, HandleType type) noexcept;

    /// True when [addr, addr+size) lies wholly inside a mapped view.
    ///
    /// VirtualAlloc needs this: pages of a SEC_RESERVE section are RESERVED by
    /// the mapping, and Microsoft Learn (CreateFileMapping, SEC_RESERVE) says
    /// they are committed by calling VirtualAlloc on them. A commit inside a
    /// view is therefore legitimate even though the range was never handed out
    /// by the VirtualAlloc reservation table.
    [[nodiscard]] bool range_within_mapped_view(GuestAddr addr,
                                                u64 size) const;

    /// Learn (VirtualAlloc MEM_RESET): "returns an error if you use MEM_RESET
    /// and the range of memory is mapped to a file. A shared view is only
    /// acceptable if it is mapped to a paging file."
    /// @return 0 when the range is not a view; ERROR_USER_MAPPED_FILE when it
    /// is a file-backed view; ~0u when it is a pagefile-backed view (reset OK).
    [[nodiscard]] u32 mem_reset_view_disposition(GuestAddr addr,
                                                 u64 size) const;

    /// Learn (SetEndOfFile): shrinking past an open file-mapping object or
    /// mapped view fails ERROR_USER_MAPPED_FILE. Growing while mapped is OK
    /// (files_core_probe SetEndOfFile_mapped_*).
    [[nodiscard]] bool blocks_shrink_end_of_file(Handle file_handle,
                                                 u64 new_size) const;

private:
    HandleTable& m_handles;
    GuestMemory& m_memory;
    FileManager* m_file_mgr{nullptr};
    ForeignFileResolver m_foreign_files;

    /// The VFS handle and guest path of a file handle from either manager.
    [[nodiscard]] u32 source_vfs_handle(Handle h_file) const;  // capsule::FileHandle
    [[nodiscard]] std::string source_path(Handle h_file) const;

    /// Section objects by handle and by name. Guest threads call
    /// CreateFileMapping / OpenFileMapping / DuplicateHandle / CloseHandle /
    /// MapViewOfFile concurrently (the kernel object manager serializes them),
    /// so both tables are read and written only under m_mappings_mutex, held
    /// for the table operation alone -- never across file I/O, shm attach or
    /// another manager's call. A peer died in map_view_impl /
    /// duplicate_handle on torn unordered_map nodes before this lock
    /// existed. Lock order when both are needed: m_views_mutex
    /// first, then m_mappings_mutex.
    std::unordered_map<Handle, std::shared_ptr<FileMappingObject>> m_mappings;
    std::unordered_map<std::string, std::weak_ptr<FileMappingObject>> m_named_mappings;
    mutable std::mutex m_mappings_mutex;
    std::unordered_map<GuestAddr, MappedView> m_views;
    mutable std::recursive_mutex m_views_mutex;

    // Prefer a high 32-bit VA for anonymous/shared views so we do not steal
    // common PE ImageBases (0x10000000 / 0x20000000 / …) that Petite and other
    // packers require. Windows MapViewOfFile without a fixed base similarly
    // avoids the low preferred-image range. The hint must still stay inside
    // GetSystemInfo lpMaximumApplicationAddress (0x7FFEFFFF for 32-bit
    // processes that are not LARGEADDRESSAWARE).
    GuestAddr m_next_view_addr{0x79000000};   ///< Hint for mapped-view allocations
    u64 m_maximum_address{0x7FFE'FFFFULL};

    [[nodiscard]] GuestAddr map_view_impl(
        Handle h_mapping, u32 desired_access, u64 offset, u64 bytes_to_map,
        GuestAddr fixed_base, MemoryProtection native_protection,
        u32 win32_protection, u64 placeholder_group);

    [[nodiscard]] bool make_mapping_transferable(
        const std::shared_ptr<FileMappingObject>& mapping);

    /// Write backing_data back to the underlying file
    void write_back_to_file(FileMappingObject& obj);

    /// Guest-visible protection of the page at @p page_offset in @p view
    /// whose stored run value is @p stored.
    [[nodiscard]] u32 report_view_protect(const MappedView& view,
                                          u64 page_offset,
                                          u32 stored) const noexcept;
};

// =============================================================================
// Thunk Registration
// =============================================================================

DllExports create_file_mapping_thunks(FileMappingManager& mgr);

/// Windows 10 placeholder-aware memory APIs that coordinate file mappings
/// with VirtualMemoryManager ownership.
DllExports create_placeholder_mapping_thunks(FileMappingManager& mgr,
                                              VirtualMemoryManager& vmm);

} // namespace vitreo::winapi
