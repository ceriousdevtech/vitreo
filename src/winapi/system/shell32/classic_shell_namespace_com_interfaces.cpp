// SPDX-License-Identifier: LGPL-2.1-only

#include "shell_paths.hpp"
#include "winapi/system/shell32/shell_paths_internal.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "winapi/ui/comctl32/comctl32.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "core/memory/guest_memory.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <cstdio>
#include <spawn.h>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

extern char** environ;

/// @file classic_shell_namespace_com_interfaces.cpp
/// @brief Classic shell namespace COM interfaces
///
/// Closed cut from shell_paths.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Classic shell namespace COM interfaces
// =============================================================================

namespace {

constexpr u32 kSOk = 0;
constexpr u32 kSFalse = 1;
constexpr u32 kENoInterface = 0x80004002u;
constexpr u32 kEPointer = 0x80004003u;
constexpr u32 kENotImpl = 0x80004001u;
constexpr u32 kEOutOfMemory = 0x8007000Eu;
constexpr u32 kSfgaoFolder = 0x20000000u;
constexpr u32 kSfgaoFileSystem = 0x40000000u;
constexpr u32 kSfgaoHasSubfolder = 0x80000000u;

/// IUnknown, IMalloc, IShellFolder and IEnumIDList all have the COM-base
/// IID form {XXXXXXXX-0000-0000-C000-000000000046}; compare all 16 bytes.
bool shell_iid_matches(Marshaler& m, GuestAddr iid, u32 data1) {
    if (iid == 0) return false;
    static constexpr u8 kTail[12] = {0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
    auto value = m.read_u32(iid);
    if (value.is_err() || value.value() != data1) return false;
    for (u32 i = 0; i < 12; ++i) {
        auto byte = m.read_u8(iid + 4 + i);
        if (byte.is_err() || byte.value() != kTail[i]) return false;
    }
    return true;
}

/// Pointer-width vtable for a Vitreo-built shell COM object: the interface
/// pointer's first slot points at `count` thunk addresses of guest width.
bool write_shell_vtable(GuestMemory& memory, ThunkRegistry& thunks,
                        GuestAddr object, bool is64,
                        const char* const* names, usize count) {
    const u32 ptr = is64 ? 8u : 4u;
    const GuestAddr vtable = object + ptr;
    auto put = [&](GuestAddr at, u64 value) {
        return is64 ? memory.write_u64(at, value).is_ok()
                    : memory.write_u32(at, static_cast<u32>(value)).is_ok();
    };
    if (!put(object, vtable)) return false;
    for (usize index = 0; index < count; ++index) {
        auto thunk = thunks.get_thunk_address("shell32.dll", names[index]);
        if (thunk.is_err() || !put(vtable + index * ptr, thunk.value())) {
            VITREO_TRACE("[shell32] shell vtable: missing thunk %s\n", names[index]);
            return false;
        }
    }
    return true;
}

std::string shell_leaf_name(const std::string& path) {
    if (path == "shell:Drives") return "My Computer";
    if (path.size() == 3 && path[1] == ':') return path.substr(0, 2);
    const usize slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

GuestAddr ShellPathManager::create_pidl(const std::string& path) {
    std::lock_guard lock(m_shell_allocation_mutex);
    // ITEMIDLISTs "are always allocated with the Component Object Model (COM)
    // task allocator" (Learn, SHGetMalloc), so ILFree, SHFree, IMalloc::Free
    // and CoTaskMemFree must all accept them.
    const u32 item_size = static_cast<u32>(2 + path.size() + 1);
    GuestAddr pidl = m_task_memory.allocate(item_size + 2);
    if (pidl == 0) return 0;

    (void)m_memory.write_u16(pidl, static_cast<u16>(item_size));
    if (!path.empty()) {
        (void)m_memory.write_bytes(pidl + 2,
                                   reinterpret_cast<const Byte*>(path.data()), path.size());
    }
    (void)m_memory.write_u8(pidl + 2 + path.size(), 0);
    (void)m_memory.write_u16(pidl + item_size, 0);
    m_pidl_paths[pidl] = std::wstring(path.begin(), path.end());
    return pidl;
}

bool ShellPathManager::pidl_path(GuestAddr pidl, std::string& path) const {
    std::lock_guard lock(m_shell_allocation_mutex);
    auto it = m_pidl_paths.find(pidl);
    if (it != m_pidl_paths.end()) {
        path.assign(it->second.begin(), it->second.end());
        return true;
    }

    auto size_result = m_memory.read_u16(pidl);
    if (size_result.is_err()) return false;
    const u16 item_size = size_result.value();
    if (item_size < 3 || !m_memory.is_mapped(pidl, static_cast<usize>(item_size) + 2)) {
        return false;
    }

    std::vector<Byte> payload(item_size - 2);
    if (m_memory.read_bytes(pidl + 2, payload.data(), payload.size()).is_err() ||
        payload.back() != 0) {
        return false;
    }
    auto terminator = std::find(payload.begin(), payload.end(), static_cast<Byte>(0));
    path.assign(reinterpret_cast<const char*>(payload.data()),
                static_cast<usize>(terminator - payload.begin()));
    return !path.empty();
}

GuestAddr ShellPathManager::create_shell_folder(const std::string& path, bool is64) {
    std::lock_guard lock(m_shell_allocation_mutex);
    if (m_thunk_registry == nullptr) {
        VITREO_TRACE("[shell32] create_shell_folder: no thunk registry\n");
        return 0;
    }
    constexpr const char* names[] = {
        "Vitreo_ShellFolder_QueryInterface", "Vitreo_ShellFolder_AddRef",
        "Vitreo_ShellFolder_Release", "Vitreo_ShellFolder_ParseDisplayName",
        "Vitreo_ShellFolder_EnumObjects", "Vitreo_ShellFolder_BindToObject",
        "Vitreo_ShellFolder_BindToStorage", "Vitreo_ShellFolder_CompareIDs",
        "Vitreo_ShellFolder_CreateViewObject", "Vitreo_ShellFolder_GetAttributesOf",
        "Vitreo_ShellFolder_GetUIObjectOf", "Vitreo_ShellFolder_GetDisplayNameOf",
        "Vitreo_ShellFolder_SetNameOf"
    };

    const u32 ptr = is64 ? 8u : 4u;
    GuestAddr object = alloc_shell_memory(m_memory, ptr + 13 * ptr);
    if (object == 0) return 0;
    if (!write_shell_vtable(m_memory, *m_thunk_registry, object, is64, names,
                            std::size(names))) {
        (void)m_memory.unmap_region(object, 0x1000);
        return 0;
    }
    m_shell_folders.emplace(object, FolderState{path, 1});
    m_shell_allocations[object] = 0x1000;
    VITREO_TRACE("[shell32] create_shell_folder: path='%s' iface=0x%llx\n",
                 path.c_str(), static_cast<unsigned long long>(object));
    return object;
}

GuestAddr ShellPathManager::create_shell_enumerator(std::vector<std::string> paths,
                                                    bool is64) {
    std::lock_guard lock(m_shell_allocation_mutex);
    if (m_thunk_registry == nullptr) return 0;
    constexpr const char* names[] = {
        "Vitreo_EnumIDList_QueryInterface", "Vitreo_EnumIDList_AddRef",
        "Vitreo_EnumIDList_Release", "Vitreo_EnumIDList_Next",
        "Vitreo_EnumIDList_Skip", "Vitreo_EnumIDList_Reset",
        "Vitreo_EnumIDList_Clone"
    };
    const u32 ptr = is64 ? 8u : 4u;
    GuestAddr object = alloc_shell_memory(m_memory, ptr + 7 * ptr);
    if (object == 0) return 0;
    if (!write_shell_vtable(m_memory, *m_thunk_registry, object, is64, names,
                            std::size(names))) {
        (void)m_memory.unmap_region(object, 0x1000);
        return 0;
    }
    m_shell_enumerators.emplace(object, EnumState{std::move(paths), 0, 1});
    m_shell_allocations[object] = 0x1000;
    return object;
}

GuestAddr ShellPathManager::create_shell_malloc(bool is64) {
    std::lock_guard lock(m_shell_allocation_mutex);
    if (m_thunk_registry == nullptr) return 0;
    constexpr const char* names[] = {
        "Vitreo_ShellMalloc_QueryInterface", "Vitreo_ShellMalloc_AddRef",
        "Vitreo_ShellMalloc_Release", "Vitreo_ShellMalloc_Alloc",
        "Vitreo_ShellMalloc_Realloc", "Vitreo_ShellMalloc_Free",
        "Vitreo_ShellMalloc_GetSize", "Vitreo_ShellMalloc_DidAlloc",
        "Vitreo_ShellMalloc_HeapMinimize"
    };
    const u32 ptr = is64 ? 8u : 4u;
    GuestAddr object = alloc_shell_memory(m_memory, ptr + 9 * ptr);
    if (object == 0) return 0;
    if (!write_shell_vtable(m_memory, *m_thunk_registry, object, is64, names,
                            std::size(names))) {
        (void)m_memory.unmap_region(object, 0x1000);
        return 0;
    }
    m_shell_allocations[object] = 0x1000;
    return object;
}

void ShellPathManager::sh_get_desktop_folder(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    GuestAddr output = m.arg_ptr(0);
    if (output == 0) { m.set_return(kEPointer); return; }
    if (m_desktop_folder == 0) m_desktop_folder = create_shell_folder("", m.cpu_is_64bit());
    if (m_desktop_folder == 0) {
        (void)m.write_ptr(output, 0);
        m.set_return(kEOutOfMemory);
        return;
    }
    ++m_shell_folders[m_desktop_folder].refs;
    (void)m.write_ptr(output, m_desktop_folder);
    VITREO_TRACE("[shell32] SHGetDesktopFolder: iface=0x%llx refs=%u\n",
                 static_cast<unsigned long long>(m_desktop_folder),
                 m_shell_folders[m_desktop_folder].refs);
    m.set_return(kSOk);
}

void ShellPathManager::sh_get_malloc(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    GuestAddr output = m.arg_ptr(0);
    if (output == 0) { m.set_return(kEPointer); return; }
    if (m_shell_malloc == 0) m_shell_malloc = create_shell_malloc(m.cpu_is_64bit());
    (void)m.write_ptr(output, m_shell_malloc);
    VITREO_TRACE("[shell32] SHGetMalloc: iface=0x%llx\n",
                 static_cast<unsigned long long>(m_shell_malloc));
    m.set_return(m_shell_malloc ? kSOk : kEOutOfMemory);
}

void ShellPathManager::shell_folder_query_interface(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    GuestAddr self = m.arg_ptr(0), iid = m.arg_ptr(1), output = m.arg_ptr(2);
    auto it = m_shell_folders.find(self);
    if (output == 0) { m.set_return(kEPointer); return; }
    (void)m.write_ptr(output, 0);
    if (it == m_shell_folders.end() ||
        (!shell_iid_matches(m, iid, 0) && !shell_iid_matches(m, iid, 0x000214E6))) {
        m.set_return(kENoInterface);
        return;
    }
    ++it->second.refs;
    (void)m.write_ptr(output, self);
    m.set_return(kSOk);
}

void ShellPathManager::shell_folder_add_ref(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    auto it = m_shell_folders.find(m.arg_ptr(0));
    m.set_return(it == m_shell_folders.end() ? 0 : ++it->second.refs);
}

void ShellPathManager::shell_folder_release(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    GuestAddr self = m.arg_ptr(0);
    auto it = m_shell_folders.find(self);
    if (it == m_shell_folders.end()) { m.set_return(0); return; }
    u32 refs = --it->second.refs;
    if (refs == 0) {
        m_shell_folders.erase(it);
        if (self == m_desktop_folder) m_desktop_folder = 0;
        auto allocation = m_shell_allocations.find(self);
        if (allocation != m_shell_allocations.end()) {
            (void)m_memory.unmap_region(self, allocation->second);
            m_shell_allocations.erase(allocation);
        }
    }
    m.set_return(refs);
}

void ShellPathManager::shell_folder_enum_objects(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    GuestAddr self = m.arg_ptr(0), output = m.arg_ptr(3);
    auto folder = m_shell_folders.find(self);
    if (output == 0) { m.set_return(kEPointer); return; }
    (void)m.write_ptr(output, 0);
    if (folder == m_shell_folders.end()) { m.set_return(hr::E_INVALIDARG); return; }

    std::vector<std::string> paths;
    if (folder->second.path.empty()) {
        paths.emplace_back("shell:Drives");
    } else if (folder->second.path == "shell:Drives") {
        for (const char drive : {'C', 'D'}) {
            std::string root = std::string(1, drive) + ":\\";
            if (m_vfs.list_dir(root).is_ok()) paths.push_back(std::move(root));
        }
    } else {
        auto listing = m_vfs.list_dir(folder->second.path);
        if (listing.is_err()) { m.set_return(hr::E_FAIL); return; }
        for (const auto& entry : listing.value()) {
            if (!capsule::has_attribute(entry.info.attributes, capsule::FileAttributes::Directory)) continue;
            std::string child = folder->second.path;
            if (!child.empty() && child.back() != '\\') child.push_back('\\');
            child += entry.name;
            paths.push_back(std::move(child));
        }
        std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
            return shell_leaf_name(left) < shell_leaf_name(right);
        });
    }

    GuestAddr enumerator = create_shell_enumerator(std::move(paths), m.cpu_is_64bit());
    if (enumerator == 0) { m.set_return(kEOutOfMemory); return; }
    VITREO_TRACE("[shell32] IShellFolder::EnumObjects: folder='%s' count=%zu enum=0x%llx\n",
                 folder->second.path.c_str(), m_shell_enumerators[enumerator].paths.size(),
                 static_cast<unsigned long long>(enumerator));
    (void)m.write_ptr(output, enumerator);
    m.set_return(kSOk);
}

void ShellPathManager::shell_folder_bind_to_object(Marshaler& m) {
    std::string path;
    GuestAddr output = m.arg_ptr(4);
    if (output == 0) { m.set_return(kEPointer); return; }
    (void)m.write_ptr(output, 0);
    if (!pidl_path(m.arg_ptr(1), path)) { m.set_return(hr::E_INVALIDARG); return; }
    GuestAddr folder = create_shell_folder(path, m.cpu_is_64bit());
    if (folder == 0) { m.set_return(kEOutOfMemory); return; }
    (void)m.write_ptr(output, folder);
    m.set_return(kSOk);
}

void ShellPathManager::shell_folder_compare_ids(Marshaler& m) {
    std::string left, right;
    if (!pidl_path(m.arg_ptr(2), left) || !pidl_path(m.arg_ptr(3), right)) {
        m.set_return(hr::E_INVALIDARG);
        return;
    }
    std::transform(left.begin(), left.end(), left.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(right.begin(), right.end(), right.begin(), [](unsigned char c) { return std::tolower(c); });
    const i16 order = left < right ? -1 : (left > right ? 1 : 0);
    m.set_return(static_cast<u16>(order));
}

void ShellPathManager::shell_folder_get_attributes(Marshaler& m) {
    const u32 count = static_cast<u32>(m.arg(1));
    GuestAddr pidls = m.arg_ptr(2), attributes_ptr = m.arg_ptr(3);
    if (count == 0 || pidls == 0 || attributes_ptr == 0) {
        m.set_return(hr::E_INVALIDARG);
        return;
    }
    u32 actual = kSfgaoFolder | kSfgaoFileSystem | kSfgaoHasSubfolder;
    for (u32 index = 0; index < count; ++index) {
        auto pidl = m.read_ptr(pidls + index * (m.cpu_is_64bit() ? 8u : 4u));
        std::string ignored;
        if (pidl.is_err() || !pidl_path(pidl.value(), ignored)) {
            m.set_return(hr::E_INVALIDARG);
            return;
        }
    }
    auto requested = m.read_u32(attributes_ptr);
    if (requested.is_err()) { m.set_return(kEPointer); return; }
    (void)m.write_u32(attributes_ptr, requested.value() == 0 ? actual : requested.value() & actual);
    m.set_return(kSOk);
}

void ShellPathManager::shell_folder_get_display_name(Marshaler& m) {
    std::string path;
    GuestAddr output = m.arg_ptr(3);
    if (output == 0) { m.set_return(kEPointer); return; }
    if (!pidl_path(m.arg_ptr(1), path)) { m.set_return(hr::E_INVALIDARG); return; }
    constexpr u32 kShgdnForParsing = 0x8000;
    const std::string name = (static_cast<u32>(m.arg(2)) & kShgdnForParsing) != 0
                                 ? path
                                 : shell_leaf_name(path);
    (void)m.write_u32(output, 2); // STRRET_CSTR
    std::vector<Byte> buffer(260, 0);
    std::memcpy(buffer.data(), name.data(), std::min<usize>(name.size(), buffer.size() - 1));
    // STRRET's union holds a pointer, so it starts at +8 on x64 (+4 on x86).
    (void)m.memory().write_bytes(output + (m.cpu_is_64bit() ? 8u : 4u),
                                 buffer.data(), buffer.size());
    m.set_return(kSOk);
}

void ShellPathManager::enum_id_list_query_interface(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    GuestAddr self = m.arg_ptr(0), iid = m.arg_ptr(1), output = m.arg_ptr(2);
    auto it = m_shell_enumerators.find(self);
    if (output == 0) { m.set_return(kEPointer); return; }
    (void)m.write_ptr(output, 0);
    if (it == m_shell_enumerators.end() ||
        (!shell_iid_matches(m, iid, 0) && !shell_iid_matches(m, iid, 0x000214F2))) {
        m.set_return(kENoInterface);
        return;
    }
    ++it->second.refs;
    (void)m.write_ptr(output, self);
    m.set_return(kSOk);
}

void ShellPathManager::enum_id_list_add_ref(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    auto it = m_shell_enumerators.find(m.arg_ptr(0));
    m.set_return(it == m_shell_enumerators.end() ? 0 : ++it->second.refs);
}

void ShellPathManager::enum_id_list_release(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    GuestAddr self = m.arg_ptr(0);
    auto it = m_shell_enumerators.find(self);
    if (it == m_shell_enumerators.end()) { m.set_return(0); return; }
    u32 refs = --it->second.refs;
    if (refs == 0) {
        m_shell_enumerators.erase(it);
        auto allocation = m_shell_allocations.find(self);
        if (allocation != m_shell_allocations.end()) {
            (void)m_memory.unmap_region(self, allocation->second);
            m_shell_allocations.erase(allocation);
        }
    }
    m.set_return(refs);
}

void ShellPathManager::enum_id_list_next(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    auto it = m_shell_enumerators.find(m.arg_ptr(0));
    u32 requested = static_cast<u32>(m.arg(1));
    GuestAddr output = m.arg_ptr(2), fetched_ptr = m.arg_ptr(3);
    if (it == m_shell_enumerators.end() || output == 0 || (requested > 1 && fetched_ptr == 0)) {
        m.set_return(hr::E_INVALIDARG);
        return;
    }
    u32 fetched = 0;
    while (fetched < requested && it->second.cursor < it->second.paths.size()) {
        GuestAddr pidl = create_pidl(it->second.paths[it->second.cursor++]);
        if (pidl == 0) break;
        (void)m.write_ptr(output + fetched * (m.cpu_is_64bit() ? 8u : 4u), pidl);
        ++fetched;
    }
    if (fetched_ptr) (void)m.write_u32(fetched_ptr, fetched);
    VITREO_TRACE("[shell32] IEnumIDList::Next: requested=%u fetched=%u cursor=%zu/%zu\n",
                 requested, fetched, it->second.cursor, it->second.paths.size());
    m.set_return(fetched == requested ? kSOk : kSFalse);
}

void ShellPathManager::enum_id_list_skip(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    auto it = m_shell_enumerators.find(m.arg_ptr(0));
    if (it == m_shell_enumerators.end()) { m.set_return(hr::E_INVALIDARG); return; }
    const usize requested = static_cast<usize>(m.arg(1));
    const usize available = it->second.paths.size() - it->second.cursor;
    it->second.cursor += std::min(requested, available);
    m.set_return(requested <= available ? kSOk : kSFalse);
}

void ShellPathManager::enum_id_list_reset(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    auto it = m_shell_enumerators.find(m.arg_ptr(0));
    if (it == m_shell_enumerators.end()) { m.set_return(hr::E_INVALIDARG); return; }
    it->second.cursor = 0;
    m.set_return(kSOk);
}

void ShellPathManager::enum_id_list_clone(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    auto it = m_shell_enumerators.find(m.arg_ptr(0));
    GuestAddr output = m.arg_ptr(1);
    if (it == m_shell_enumerators.end() || output == 0) { m.set_return(hr::E_INVALIDARG); return; }
    const usize cursor = it->second.cursor;
    GuestAddr clone = create_shell_enumerator(it->second.paths, m.cpu_is_64bit());
    if (clone == 0) { m.set_return(kEOutOfMemory); return; }
    m_shell_enumerators[clone].cursor = cursor;
    (void)m.write_ptr(output, clone);
    m.set_return(kSOk);
}

void ShellPathManager::shell_malloc_query_interface(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    GuestAddr iid = m.arg_ptr(1), output = m.arg_ptr(2);
    if (output == 0) { m.set_return(kEPointer); return; }
    (void)m.write_ptr(output, 0);
    if (!shell_iid_matches(m, iid, 0) && !shell_iid_matches(m, iid, 2)) {
        m.set_return(kENoInterface);
        return;
    }
    (void)m.write_ptr(output, m_shell_malloc);
    m.set_return(kSOk);
}
void ShellPathManager::shell_malloc_add_ref(Marshaler& m) { (void)m.arg(0); m.set_return(2); }
void ShellPathManager::shell_malloc_release(Marshaler& m) { (void)m.arg(0); m.set_return(1); }

// SHGetMalloc's IMalloc is the COM task allocator (Learn: "the Shell's
// allocator ... is the same as CoGetMalloc"), so blocks cross freely between
// IMalloc, SHFree, ILFree and CoTaskMemFree. SIZE_T args are pointer-width.
void ShellPathManager::shell_malloc_alloc(Marshaler& m) {
    m.set_return(m_task_memory.allocate(m.arg(1)));
}

void ShellPathManager::shell_malloc_realloc(Marshaler& m) {
    std::lock_guard lock(m_shell_allocation_mutex);
    const GuestAddr old = m.arg_ptr(1);
    const u64 size = m.arg(2);
    if (old != 0 && size == 0) m_pidl_paths.erase(old);
    const GuestAddr moved = m_task_memory.reallocate(old, size);
    if (moved != 0 && moved != old) {
        auto path = m_pidl_paths.find(old);
        if (path != m_pidl_paths.end()) {
            auto saved = std::move(path->second);
            m_pidl_paths.erase(path);
            m_pidl_paths[moved] = std::move(saved);
        }
    }
    m.set_return(moved);
}

void ShellPathManager::shell_malloc_free(Marshaler& m) {
    sh_free(m.arg_ptr(1));
    m.set_return(0);
}

void ShellPathManager::shell_malloc_get_size(Marshaler& m) {
    // IMalloc::GetSize returns SIZE_T -1 for a block this allocator did not
    // hand out.
    const auto size = m_task_memory.allocation_size(m.arg_ptr(1));
    m.set_return(size ? *size : (m.cpu_is_64bit() ? ~u64{0} : 0xFFFFFFFFull));
}
void ShellPathManager::shell_malloc_did_alloc(Marshaler& m) {
    // 1 = allocated here, 0 = not; -1 is reserved for "cannot tell".
    const GuestAddr block = m.arg_ptr(1);
    m.set_return(block != 0 && m_task_memory.owns(block) ? 1 : 0);
}
void ShellPathManager::shell_malloc_heap_minimize(Marshaler& m) { (void)m.arg(0); m.set_return(0); }

/// Guest path -> host path for anything handed to NSWorkspace.
///
/// Returns empty when the path cannot be resolved, which callers must treat as
/// SE_ERR_FNF rather than pretending success -- an "Open Directory" button
/// that reports success and does nothing is strictly worse than one that
/// fails, because nothing upstream ever learns.
std::string ShellPathManager::host_path_for_shell(const std::string& guest_path) {
    if (guest_path.empty()) return {};
    // Already a host path (a POSIX absolute path): pass it through.
    if (guest_path[0] == '/') return guest_path;
    auto resolved = m_vfs.resolve_host_path(guest_path);
    if (resolved.is_ok() && !resolved.value().empty()) {
        return std::move(resolved.value());
    }
    return {};
}

u64 ShellPathManager::shell_execute(const std::string& operation,
                                    const std::string& file,
                                    const std::string& parameters,
                                    const std::string& directory,
                                    i32 show_cmd,
                                    u32* process_handle,
                                    const std::string& class_name) {
    VITREO_TRACE("[shell32] ShellExecuteW: op='%s' file='%s' params='%s' dir='%s' show=%d\n",
                 operation.c_str(), file.c_str(), parameters.c_str(),
                 directory.c_str(), show_cmd);
    std::fprintf(stderr,
                 "[vitreo] ShellExecute op='%s' file='%s' params='%s'\n",
                 operation.c_str(), file.c_str(), parameters.c_str());

    // Every failure reports the Win32 reason through the last error and the
    // matching SE_ERR_* in hInstApp (shell_paths_probe see_missing*: gle=2,
    // hInstApp=2 for a missing file or directory).
    auto fail = [](u32 win32_error, u64 se_err) -> u64 {
        set_last_error(win32_error);
        return se_err;
    };
    constexpr u64 SE_ERR_FNF = 2, SE_ERR_ACCESSDENIED = 5, SE_ERR_OOM = 8,
                  SE_ERR_NOASSOC = 31;
    if (file.empty()) {
        return fail(error_codes::ERROR_FILE_NOT_FOUND, SE_ERR_FNF);
    }
    if (process_handle != nullptr) {
        *process_handle = 0;
    }

    // Normalise operation to lowercase for comparison
    std::string op = operation;
    std::transform(op.begin(), op.end(), op.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // --- URLs: http://, https://, mailto: → delegate to NSWorkspace ---
    if (file.rfind("http://", 0) == 0 ||
        file.rfind("https://", 0) == 0 ||
        file.rfind("mailto:", 0) == 0 ||
        file.rfind("steam://", 0) == 0) {
        if (!op.empty() && op != "open") {
            return fail(1155u /*ERROR_NO_ASSOCIATION*/, SE_ERR_NOASSOC);
        }
        bool ok = m_bridge.shell_open(file);
        VITREO_TRACE("[shell32] ShellExecuteW: opened URL -> %s\n", ok ? "success" : "failed");
        return ok ? 42u : fail(error_codes::ERROR_FILE_NOT_FOUND, SE_ERR_FNF);
    }

    // --- "explore" / "find" on a directory path ---
    //
    // This used to log "(no-op in emulator)" and return 42 (success). An app
    // whose "Open Directory" button does nothing at all, while every error
    // path it can see reports success, is the result -- measured on Cerious
    // AASM, whose Open Directory buttons are exactly this call.
    //
    // Explore/find mean "show me this folder", which is what Finder does when
    // handed a directory, so both map onto the same open.
    if (op == "explore" || op == "find") {
        const std::string host = host_path_for_shell(file);
        if (host.empty()) {
            VITREO_TRACE("[shell32] ShellExecuteW: explore '%s' -> no host path\n",
                         file.c_str());
            return fail(error_codes::ERROR_FILE_NOT_FOUND, SE_ERR_FNF);
        }
        const bool ok = m_bridge.shell_open(host);
        VITREO_TRACE("[shell32] ShellExecuteW: explore '%s' -> host '%s' %s\n",
                     file.c_str(), host.c_str(), ok ? "ok" : "FAILED");
        return ok ? 42u : fail(error_codes::ERROR_FILE_NOT_FOUND, SE_ERR_FNF);
    }

    // --- "open" (or default) on a file ---
    // Check for Windows executable extensions — we can't launch those natively
    auto has_ext = [&](const char* ext) {
        if (file.size() < std::strlen(ext)) return false;
        std::string tail = file.substr(file.size() - std::strlen(ext));
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return tail == ext;
    };

    // SEE_MASK_CLASSNAME: lpClass names the file type instead of the
    // extension (".exe" or a ProgID such as "exefile").
    std::string cls = class_name;
    std::transform(cls.begin(), cls.end(), cls.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const bool class_given = !cls.empty();
    const bool class_is_program =
        cls == ".exe" || cls == ".bat" || cls == ".cmd" || cls == ".com" ||
        cls == "exefile" || cls == "batfile" || cls == "cmdfile" || cls == "comfile";
    if (class_given ? class_is_program
                    : (has_ext(".exe") || has_ext(".bat") || has_ext(".cmd") ||
                       has_ext(".com"))) {
        // The http association names the host's default browser. That exe is
        // not in the capsule; launching it runs the same host app.
        if ((op.empty() || op == "open") &&
            vitreo::windowing::launch_host_http_handler(file, parameters)) {
            return 42u;
        }
        // The exe class registers open (the default) and runas; any other
        // verb has no command (ERROR_NO_ASSOCIATION / SE_ERR_NOASSOC).
        if (!op.empty() && op != "open" && op != "runas") {
            return fail(1155u /*ERROR_NO_ASSOCIATION*/, SE_ERR_NOASSOC);
        }
        auto host_path_res = m_vfs.resolve_host_path(file);
        std::string launch_path;
        if (host_path_res.is_ok()) {
            launch_path = std::move(host_path_res.value());
        } else {
            auto info = m_vfs.stat(file);
            if (info.is_err() || capsule::has_attribute(
                    info.value().attributes, capsule::FileAttributes::Directory)) {
                VITREO_TRACE("[shell32] ShellExecuteW: resolve_path failed for '%s'\n", file.c_str());
                return fail(error_codes::ERROR_FILE_NOT_FOUND, SE_ERR_FNF);
            }
            // The run pipeline can load a drive-qualified executable directly
            // from a non-host-backed filesystem such as a mounted ISO image.
            launch_path = file;
        }

        if (!m_executable_launcher) {
            VITREO_TRACE("[shell32] ShellExecuteW: no executable launcher for '%s'\n",
                         file.c_str());
            return fail(error_codes::ERROR_ACCESS_DENIED, SE_ERR_ACCESSDENIED);
        }

        u32 launched_process = 0;
        u32 launch_error = error_codes::ERROR_ACCESS_DENIED;
        const bool request_elevation = op == "runas";
        if (!m_executable_launcher(
                file, parameters, directory, show_cmd, request_elevation,
                launched_process, launch_error)) {
            VITREO_TRACE(
                "[shell32] ShellExecuteW: child launch failed for '%s' "
                "runas=%d error=%u\n",
                file.c_str(), request_elevation ? 1 : 0, launch_error);
            std::fprintf(stderr,
                         "[vitreo] ShellExecute launch failed file='%s' error=%u\n",
                         file.c_str(), launch_error);
            // hInstApp mirrors the reason (SE_ERR_* equal the Win32 codes
            // for file/path not found; a cancelled or refused elevation is
            // SE_ERR_ACCESSDENIED).
            u64 se = SE_ERR_ACCESSDENIED;
            if (launch_error == error_codes::ERROR_FILE_NOT_FOUND) se = SE_ERR_FNF;
            else if (launch_error == error_codes::ERROR_PATH_NOT_FOUND) se = 3;
            else if (launch_error == error_codes::ERROR_NOT_ENOUGH_MEMORY) se = SE_ERR_OOM;
            else if (launch_error == 11u /*ERROR_BAD_FORMAT*/) se = 11;
            return fail(launch_error, se);
        }
        if (process_handle != nullptr) {
            *process_handle = launched_process;
        }
        VITREO_TRACE("[shell32] ShellExecuteW: launched child '%s' hProc=0x%x\n",
                     launch_path.c_str(), launched_process);
        std::fprintf(stderr,
                     "[vitreo] ShellExecute launched '%s' hProc=0x%x\n",
                     file.c_str(), launched_process);
        return 42u; // success
    }

    // Everything else -- a directory, a document, an image -- goes to
    // NSWorkspace. It must be a HOST path: NSWorkspace has no notion of a C:
    // drive, and handing it a guest path silently opened nothing.
    // Only the default open verb has a host equivalent (NSWorkspace opens a
    // document in its default app); edit / print / properties / openas have
    // no registered command here.
    if (!op.empty() && op != "open") {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED shell32!ShellExecuteEx: verb '%s' on a document\n",
            op.c_str());
        return fail(1155u /*ERROR_NO_ASSOCIATION*/, SE_ERR_NOASSOC);
    }
    const std::string host = host_path_for_shell(file);
    if (host.empty() || (file[0] != '/' && m_vfs.stat(file).is_err())) {
        VITREO_TRACE("[shell32] ShellExecuteW: '%s' -> no host path\n",
                     file.c_str());
        return fail(error_codes::ERROR_FILE_NOT_FOUND, SE_ERR_FNF);
    }
    bool ok = m_bridge.shell_open(host);
    VITREO_TRACE("[shell32] ShellExecuteW: shell_open('%s' -> '%s') -> %s\n",
                 file.c_str(), host.c_str(), ok ? "success" : "failed");
    return ok ? 42u : fail(1155u /*ERROR_NO_ASSOCIATION*/, SE_ERR_NOASSOC);
}

std::string ShellPathManager::expand_environment(const std::string& text) const {
    std::string out;
    usize i = 0;
    while (i < text.size()) {
        const usize open = text.find('%', i);
        if (open == std::string::npos) { out += text.substr(i); break; }
        const usize close = text.find('%', open + 1);
        if (close == std::string::npos) { out += text.substr(i); break; }
        out += text.substr(i, open - i);
        const std::string name = text.substr(open + 1, close - open - 1);
        std::optional<std::string> value;
        if (!name.empty() && m_environment_lookup) value = m_environment_lookup(name);
        if (value) {
            out += *value;
            i = close + 1;
        } else {
            out += '%';          // not a variable: keep it and rescan from
            i = open + 1;        // the second '%'
        }
    }
    return out;
}

u32 ShellPathManager::is_user_an_admin() const {
    const bool elevated =
        m_elevation != nullptr && m_elevation->is_elevated();
    VITREO_TRACE("[shell32] IsUserAnAdmin (ordinal 680): returning %s\n",
                 elevated ? "TRUE" : "FALSE");
    return elevated ? 1u : 0u;
}

} // namespace vitreo::winapi
