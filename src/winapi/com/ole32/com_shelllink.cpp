// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_shelllink.cpp
/// @brief IShellLink, IPersistFile and IPropertyStore objects (CLSID_ShellLink).
///
/// IPropertyStore behaviour and the PropertyStoreDataBlock bytes are measured:
/// tools/oracle_probe/shelllink_propstore_probe.c ->
/// shelllink_propstore_probe{32,64}.expected.

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "com_core_internal.hpp"
#include "com_wasapi_internal.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

namespace vitreo::winapi {

namespace {

[[nodiscard]] bool ensure_parent_dirs(capsule::Vfs& vfs, const std::string& win_path) {
    auto parsed = capsule::parse_windows_path(win_path);
    if (!parsed.drive_letter) {
        return false;
    }
    std::string rel = parsed.relative_path;
    auto slash = rel.find_last_of('/');
    if (slash == std::string::npos) {
        return true;
    }
    rel = rel.substr(0, slash);

    std::string accum;
    usize start = 0;
    while (start <= rel.size()) {
        usize next = rel.find('/', start);
        std::string part = (next == std::string::npos)
            ? rel.substr(start)
            : rel.substr(start, next - start);
        if (!part.empty()) {
            if (!accum.empty()) accum.push_back('\\');
            accum += part;
            std::string cur = std::string(1, capsule::normalize_drive_letter(parsed.drive_letter)) + ":\\" + accum;
            auto r = vfs.create_dir(cur);
            if (r.is_err() && r.error() != capsule::VfsError::PathAlreadyExists) {
                return false;
            }
        }
        if (next == std::string::npos) {
            break;
        }
        start = next + 1;
    }
    return true;
}

} // namespace

u32 ComCoreManager::shelllink_query_interface(Marshaler& m) {
    std::lock_guard lock(m_shell_links_mutex);
    const GuestAddr this_ptr = m.arg_ptr(0);
    const GuestAddr riid_ptr = m.arg_ptr(1);
    const GuestAddr ppv_ptr = m.arg_ptr(2);
    if (riid_ptr == 0 || ppv_ptr == 0) return com::E_INVALIDARG;
    auto it = m_iface_to_shelllink.find(this_ptr);
    if (it == m_iface_to_shelllink.end()) return E_NOINTERFACE;
    auto sid = it->second.shell_link_id;
    auto st_it = m_shell_links.find(sid);
    if (st_it == m_shell_links.end()) return E_NOINTERFACE;
    auto iid_r = m.read_struct<Guid>(riid_ptr);
    if (iid_r.is_err()) return com::E_INVALIDARG;
    const Guid& iid = iid_r.value();
    ipc::ipc_log("ShellLink QueryInterface this=0x%llx iid=%s",
                 static_cast<unsigned long long>(this_ptr),
                 format_guid(iid).c_str());

    GuestAddr out = 0;
    if (guid_equal(iid, IID_IUNKNOWN) || guid_equal(iid, IID_ISHELLLINKW)) {
        out = st_it->second.unicode_iface;
    } else if (guid_equal(iid, IID_ISHELLLINKA)) {
        out = st_it->second.ansi_iface;
    } else if (guid_equal(iid, IID_IPERSISTFILE)) {
        out = st_it->second.persist_iface;
    } else if (guid_equal(iid, IID_IPROPERTYSTORE)) {
        out = st_it->second.propstore_iface;
    } else {
        (void)m.write_ptr(ppv_ptr, 0);
        return E_NOINTERFACE;
    }
    st_it->second.ref_count++;
    if (m.write_ptr(ppv_ptr, out).is_err()) {
        return com::E_INVALIDARG;
    }
    ipc::ipc_log("ShellLink QueryInterface -> 0x%llx",
                 static_cast<unsigned long long>(out));
    return com::S_OK;
}

u32 ComCoreManager::shelllink_add_ref(Marshaler& m) {
    std::lock_guard lock(m_shell_links_mutex);
    auto it = m_iface_to_shelllink.find(m.arg_ptr(0));
    if (it == m_iface_to_shelllink.end()) return 1;
    auto& st = m_shell_links[it->second.shell_link_id];
    st.ref_count++;
    return st.ref_count;
}

u32 ComCoreManager::shelllink_release(Marshaler& m) {
    std::lock_guard lock(m_shell_links_mutex);
    auto fit = m_iface_to_shelllink.find(m.arg_ptr(0));
    if (fit == m_iface_to_shelllink.end()) return 0;
    u32 sid = fit->second.shell_link_id;
    auto sit = m_shell_links.find(sid);
    if (sit == m_shell_links.end()) return 0;
    if (sit->second.ref_count > 0) sit->second.ref_count--;
    u32 rc = sit->second.ref_count;
    if (rc == 0) {
        m_iface_to_shelllink.erase(sit->second.ansi_iface);
        m_iface_to_shelllink.erase(sit->second.unicode_iface);
        m_iface_to_shelllink.erase(sit->second.persist_iface);
        m_iface_to_shelllink.erase(sit->second.propstore_iface);
        for (const auto& prop : sit->second.properties) {
            (void)prop_variant_clear_at(m, prop.value);
            co_task_mem_free_guest(prop.value);
        }
        m_shell_links.erase(sit);
    }
    ipc::ipc_log("ShellLink Release this=0x%llx -> %u",
                 static_cast<unsigned long long>(m.arg_ptr(0)), rc);
    return rc;
}

bool ComCoreManager::read_shelllink_string(
    Marshaler& m,
    const ShellLinkInterfaceBinding& binding,
    GuestAddr string_ptr,
    std::string& value) const {
    if (binding.kind == ShellLinkInterfaceKind::PersistFile) {
        return false;
    }
    auto result = binding.kind == ShellLinkInterfaceKind::Ansi
        ? m.read_ansi_string(string_ptr)
        : m.read_wide_string(string_ptr);
    if (result.is_err()) {
        return false;
    }
    value = std::move(result.value());
    return true;
}

u32 ComCoreManager::shelllink_set_path(Marshaler& m) {
    std::lock_guard lock(m_shell_links_mutex);
    auto it = m_iface_to_shelllink.find(m.arg_ptr(0));
    std::string value;
    if (it == m_iface_to_shelllink.end() ||
        !read_shelllink_string(m, it->second, m.arg_ptr(1), value)) {
        return com::E_INVALIDARG;
    }
    m_shell_links[it->second.shell_link_id].target_path = std::move(value);
    ipc::ipc_log("ShellLink SetPath '%s'",
                 m_shell_links[it->second.shell_link_id].target_path.c_str());
    return com::S_OK;
}

u32 ComCoreManager::shelllink_get_path(Marshaler& m) {
    std::lock_guard lock(m_shell_links_mutex);
    auto it = m_iface_to_shelllink.find(m.arg_ptr(0));
    if (it == m_iface_to_shelllink.end() ||
        it->second.kind == ShellLinkInterfaceKind::PersistFile) {
        return com::E_INVALIDARG;
    }
    const GuestAddr buffer = m.arg_ptr(1);
    const i32 cch = static_cast<i32>(m.arg(2));
    if (buffer == 0 || cch <= 0) return com::E_INVALIDARG;

    const auto& st = m_shell_links[it->second.shell_link_id];
    const bool ansi = it->second.kind == ShellLinkInterfaceKind::Ansi;
    const usize max_chars = static_cast<usize>(cch);
    if (st.target_path.empty()) {
        if (ansi) {
            if (m.write_u8(buffer, 0).is_err()) return com::E_INVALIDARG;
        } else if (m.write_u16(buffer, 0).is_err()) {
            return com::E_INVALIDARG;
        }
        return com::S_FALSE;
    }

    if (ansi) {
        const usize copy = std::min(st.target_path.size(), max_chars - 1u);
        if (m.memory().write_bytes(
                buffer, reinterpret_cast<const Byte*>(st.target_path.data()),
                copy).is_err() ||
            m.write_u8(buffer + copy, 0).is_err()) {
            return com::E_INVALIDARG;
        }
    } else {
        const std::vector<u16> wide = utf8_to_utf16(st.target_path);
        const usize copy = std::min(wide.size(), max_chars - 1u);
        for (usize i = 0; i < copy; ++i) {
            if (m.write_u16(buffer + i * 2u, wide[i]).is_err()) {
                return com::E_INVALIDARG;
            }
        }
        if (m.write_u16(buffer + copy * 2u, 0).is_err()) {
            return com::E_INVALIDARG;
        }
    }

    const GuestAddr find_data = m.arg_ptr(3);
    if (find_data != 0) {
        const usize find_size = ansi ? 320u : 592u;
        std::vector<Byte> zeros(find_size, 0);
        (void)m.memory().write_bytes(find_data, zeros.data(), zeros.size());
    }
    ipc::ipc_log("ShellLink GetPath '%s'", st.target_path.c_str());
    return com::S_OK;
}

u32 ComCoreManager::shelllink_set_arguments(Marshaler& m) {
    std::lock_guard lock(m_shell_links_mutex);
    auto it = m_iface_to_shelllink.find(m.arg_ptr(0));
    std::string value;
    if (it == m_iface_to_shelllink.end() ||
        !read_shelllink_string(m, it->second, m.arg_ptr(1), value)) {
        return com::E_INVALIDARG;
    }
    m_shell_links[it->second.shell_link_id].arguments = std::move(value);
    return com::S_OK;
}

u32 ComCoreManager::shelllink_set_working_directory(Marshaler& m) {
    std::lock_guard lock(m_shell_links_mutex);
    auto it = m_iface_to_shelllink.find(m.arg_ptr(0));
    std::string value;
    if (it == m_iface_to_shelllink.end() ||
        !read_shelllink_string(m, it->second, m.arg_ptr(1), value)) {
        return com::E_INVALIDARG;
    }
    m_shell_links[it->second.shell_link_id].working_dir = std::move(value);
    return com::S_OK;
}

u32 ComCoreManager::shelllink_set_description(Marshaler& m) {
    std::lock_guard lock(m_shell_links_mutex);
    auto it = m_iface_to_shelllink.find(m.arg_ptr(0));
    std::string value;
    if (it == m_iface_to_shelllink.end() ||
        !read_shelllink_string(m, it->second, m.arg_ptr(1), value)) {
        return com::E_INVALIDARG;
    }
    m_shell_links[it->second.shell_link_id].description = std::move(value);
    return com::S_OK;
}

u32 ComCoreManager::shelllink_set_icon_location(Marshaler& m) {
    std::lock_guard lock(m_shell_links_mutex);
    auto it = m_iface_to_shelllink.find(m.arg_ptr(0));
    std::string value;
    if (it == m_iface_to_shelllink.end() ||
        !read_shelllink_string(m, it->second, m.arg_ptr(1), value)) {
        return com::E_INVALIDARG;
    }
    m_shell_links[it->second.shell_link_id].icon_path = std::move(value);
    return com::S_OK;
}

u32 ComCoreManager::shelllink_set_show_cmd(Marshaler& m) {
    std::lock_guard lock(m_shell_links_mutex);
    auto it = m_iface_to_shelllink.find(m.arg_ptr(0));
    if (it == m_iface_to_shelllink.end()) return com::E_INVALIDARG;
    if (it->second.kind == ShellLinkInterfaceKind::PersistFile) {
        return com::E_INVALIDARG;
    }
    m_shell_links[it->second.shell_link_id].show_cmd = static_cast<i32>(m.arg(1));
    return com::S_OK;
}

u32 ComCoreManager::shelllink_persist_save(Marshaler& m) {
    ShellLinkState st;
    {
        std::lock_guard lock(m_shell_links_mutex);
        auto it = m_iface_to_shelllink.find(m.arg_ptr(0));
        if (it == m_iface_to_shelllink.end()) return com::E_INVALIDARG;
        auto state = m_shell_links.find(it->second.shell_link_id);
        if (state == m_shell_links.end()) return com::E_INVALIDARG;
        st = state->second;
    }
    auto path_r = m.read_wide_string(m.arg_ptr(1));
    if (path_r.is_err() || path_r.value().empty() || m_vfs == nullptr) {
        VITREO_TRACE("[ole32] IPersistFile::Save invalid args\n");
        return com::E_INVALIDARG;
    }
    const std::string save_path = path_r.value();
    ipc::ipc_log("ShellLink IPersistFile::Save '%s'", save_path.c_str());

    if (!ensure_parent_dirs(*m_vfs, save_path)) {
        VITREO_TRACE("[ole32] IPersistFile::Save ensure_parent_dirs failed: %s\n", save_path.c_str());
        return com::E_INVALIDARG;
    }

    std::vector<u8> payload;
    const auto append_u16 = [&](u16 value) {
        payload.push_back(static_cast<u8>(value));
        payload.push_back(static_cast<u8>(value >> 8));
    };
    const auto append_u32 = [&](u32 value) {
        for (u32 shift = 0; shift < 32; shift += 8) {
            payload.push_back(static_cast<u8>(value >> shift));
        }
    };
    const auto append_u64 = [&](u64 value) {
        for (u32 shift = 0; shift < 64; shift += 8) {
            payload.push_back(static_cast<u8>(value >> shift));
        }
    };
    const auto append_guid = [&](const Guid& value) {
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        payload.insert(payload.end(), bytes, bytes + sizeof(value));
    };
    const auto append_string_data = [&](std::string_view value) {
        const std::vector<u16> wide = utf8_to_utf16(value);
        append_u16(static_cast<u16>(
            std::min<usize>(wide.size(), std::numeric_limits<u16>::max())));
        for (const u16 character : wide) append_u16(character);
    };

    // [MS-SHLLINK] 2.1: a Shell Link is a 0x4C-byte header followed by
    // optional structures selected by LinkFlags. Use Unicode StringData for
    // the target and the optional IShellLink properties we currently retain.
    constexpr u32 HAS_NAME = 0x00000004u;
    constexpr u32 HAS_RELATIVE_PATH = 0x00000008u;
    constexpr u32 HAS_WORKING_DIR = 0x00000010u;
    constexpr u32 HAS_ARGUMENTS = 0x00000020u;
    constexpr u32 HAS_ICON_LOCATION = 0x00000040u;
    constexpr u32 IS_UNICODE = 0x00000080u;
    u32 link_flags = IS_UNICODE | HAS_RELATIVE_PATH;
    if (!st.description.empty()) link_flags |= HAS_NAME;
    if (!st.working_dir.empty()) link_flags |= HAS_WORKING_DIR;
    if (!st.arguments.empty()) link_flags |= HAS_ARGUMENTS;
    if (!st.icon_path.empty()) link_flags |= HAS_ICON_LOCATION;

    append_u32(0x4Cu);
    append_guid(CLSID_SHELLLINK);
    append_u32(link_flags);
    append_u32(0);  // FileAttributes
    append_u64(0);  // CreationTime
    append_u64(0);  // AccessTime
    append_u64(0);  // WriteTime
    append_u32(0);  // FileSize
    append_u32(0);  // IconIndex
    append_u32(st.show_cmd == 0 ? 1u : static_cast<u32>(st.show_cmd));
    append_u16(0);  // HotKey
    append_u16(0);  // Reserved1
    append_u32(0);  // Reserved2
    append_u32(0);  // Reserved3
    if (!st.description.empty()) append_string_data(st.description);
    append_string_data(st.target_path);
    if (!st.working_dir.empty()) append_string_data(st.working_dir);
    if (!st.arguments.empty()) append_string_data(st.arguments);
    if (!st.icon_path.empty()) append_string_data(st.icon_path);
    // ExtraData: the PropertyStoreDataBlock, then the TerminalBlock.
    const std::vector<u8> props = shelllink_property_block(m, st.properties);
    payload.insert(payload.end(), props.begin(), props.end());
    append_u32(0);  // TerminalBlock

    auto h = m_vfs->open(save_path,
                         capsule::AccessMode::Write,
                         capsule::ShareMode::Read,
                         capsule::CreationDisposition::CreateAlways);
    if (h.is_err()) {
        VITREO_TRACE("[ole32] IPersistFile::Save open failed: %s\n", save_path.c_str());
        return com::E_INVALIDARG;
    }
    const auto write_result = m_vfs->write(h.value(), payload);
    const auto close_result = m_vfs->close(h.value());
    if (write_result.is_err() || write_result.value() != payload.size() ||
        close_result.is_err()) {
        ipc::ipc_log(
            "ShellLink IPersistFile::Save failed write_ok=%d written=%llu "
            "expected=%llu close_ok=%d",
            write_result.is_ok() ? 1 : 0,
            static_cast<unsigned long long>(
                write_result.is_ok() ? write_result.value() : 0),
            static_cast<unsigned long long>(payload.size()),
            close_result.is_ok() ? 1 : 0);
        return com::S_FALSE;
    }
    VITREO_TRACE("[ole32] IPersistFile::Save wrote shortcut: %s -> target=%s\n",
                 save_path.c_str(), st.target_path.c_str());
    ipc::ipc_log("ShellLink IPersistFile::Save -> S_OK bytes=%llu",
                 static_cast<unsigned long long>(payload.size()));
    return com::S_OK;
}

u32 ComCoreManager::shelllink_persist_load(Marshaler& /*m*/) {
    // Learn IPersistFile::Load: success means the file initialized the object.
    // No Shell Link deserializer exists yet, for any access mode or bitness.
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!reported.test_and_set(std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED ole32!IPersistFile::Load: "
            "Shell Link deserialization (all access modes)\n");
    }
    return E_FAIL;
}

u32 ComCoreManager::shelllink_persist_is_dirty(Marshaler& /*m*/) { return com::S_FALSE; }
u32 ComCoreManager::shelllink_persist_get_class_id(Marshaler& m) {
    ipc::ipc_log("ShellLink IPersist::GetClassID");
    GuestAddr pclsid = m.arg_ptr(1);
    if (pclsid == 0) return com::E_INVALIDARG;
    auto wr = m.write_struct<Guid>(pclsid, CLSID_SHELLLINK);
    return wr.is_ok() ? com::S_OK : com::E_INVALIDARG;
}
u32 ComCoreManager::shelllink_persist_save_completed(Marshaler& m) {
    auto path = m.read_wide_string(m.arg_ptr(1));
    ipc::ipc_log("ShellLink IPersistFile::SaveCompleted '%s'",
                 path.is_ok() ? path.value().c_str() : "(null)");
    return com::S_OK;
}
u32 ComCoreManager::shelllink_persist_get_cur_file(Marshaler& m) {
    ipc::ipc_log("ShellLink IPersistFile::GetCurFile");
    GuestAddr ppsz = m.arg_ptr(1);
    if (ppsz != 0) (void)m.write_u32(ppsz, 0);
    return com::S_FALSE;
}

// =============================================================================
// IPropertyStore (propsys.h) on CLSID_ShellLink. Measured
// (shelllink_propstore_probe): SetValue accepts every PROPVARIANT type and
// stores a copy (a later SetValue replaces type and value); VT_EMPTY keeps the
// key in GetCount but nothing is saved for it; GetValue of an unset key is
// S_OK with VT_EMPTY; GetAt out of range zeroes the key and returns
// E_INVALIDARG; Commit is S_OK and is not needed before IPersistFile::Save.
// =============================================================================

namespace {

constexpr u32 kPropertyKeySize = 20;  // GUID fmtid + DWORD pid

struct PropertyKey {
    Guid fmtid{};
    u32 pid{0};
};

[[nodiscard]] bool read_property_key(Marshaler& m, GuestAddr at, PropertyKey& key) {
    if (at == 0) return false;
    auto g = m.read_struct<Guid>(at);
    auto p = m.read_u32(at + 16);
    if (g.is_err() || p.is_err()) return false;
    key.fmtid = g.value();
    key.pid = p.value();
    return true;
}

}  // namespace

#define VITREO_SHELLLINK_PROPS_OR_RETURN(props_var)                               \
    std::lock_guard lock(m_shell_links_mutex);                                    \
    auto bind_it = m_iface_to_shelllink.find(m.arg_ptr(0));                       \
    if (bind_it == m_iface_to_shelllink.end()) return com::E_INVALIDARG;          \
    auto link_it = m_shell_links.find(bind_it->second.shell_link_id);             \
    if (link_it == m_shell_links.end()) return com::E_INVALIDARG;                 \
    auto& props_var = link_it->second.properties

u32 ComCoreManager::shelllink_propstore_get_count(Marshaler& m) {
    VITREO_SHELLLINK_PROPS_OR_RETURN(props);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    return m.write_u32(out, static_cast<u32>(props.size())).is_ok() ? com::S_OK
                                                                      : com::E_POINTER;
}

u32 ComCoreManager::shelllink_propstore_get_at(Marshaler& m) {
    VITREO_SHELLLINK_PROPS_OR_RETURN(props);
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    if (index >= props.size()) {
        const std::vector<u8> zeros(kPropertyKeySize, 0);
        (void)m.memory().write_bytes(out, zeros.data(), zeros.size());
        return com::E_INVALIDARG;
    }
    (void)m.write_struct<Guid>(out, props[index].fmtid);
    (void)m.write_u32(out + 16, props[index].pid);
    return com::S_OK;
}

u32 ComCoreManager::shelllink_propstore_get_value(Marshaler& m) {
    PropertyKey key;
    const GuestAddr out = m.arg_ptr(2);
    if (!read_property_key(m, m.arg_ptr(1), key) || out == 0) return com::E_POINTER;
    GuestAddr stored = 0;
    {
        VITREO_SHELLLINK_PROPS_OR_RETURN(props);
        for (const auto& prop : props) {
            if (guid_equal(prop.fmtid, key.fmtid) && prop.pid == key.pid) {
                stored = prop.value;
                break;
            }
        }
    }
    if (stored == 0) {
        // PropVariantInit: VT_EMPTY, S_OK.
        const std::vector<u8> zeros(m.cpu_is_64bit() ? 24u : 16u, 0);
        (void)m.memory().write_bytes(out, zeros.data(), zeros.size());
        return com::S_OK;
    }
    return prop_variant_copy_at(m, out, stored);
}

u32 ComCoreManager::shelllink_propstore_set_value(Marshaler& m) {
    PropertyKey key;
    const GuestAddr src = m.arg_ptr(2);
    if (!read_property_key(m, m.arg_ptr(1), key) || src == 0) return com::E_POINTER;
    const u32 pv_size = m.cpu_is_64bit() ? 24u : 16u;
    const GuestAddr copy = co_task_mem_alloc_guest(pv_size);
    if (copy == 0) return E_OUTOFMEMORY;
    const u32 hr = prop_variant_copy_at(m, copy, src);
    if ((hr & 0x80000000u) != 0) {
        co_task_mem_free_guest(copy);
        return hr;
    }
    GuestAddr previous = 0;
    {
        VITREO_SHELLLINK_PROPS_OR_RETURN(props);
        bool replaced = false;
        for (auto& prop : props) {
            if (guid_equal(prop.fmtid, key.fmtid) && prop.pid == key.pid) {
                previous = prop.value;
                prop.value = copy;
                replaced = true;
                break;
            }
        }
        if (!replaced) props.push_back({key.fmtid, key.pid, copy});
    }
    if (previous != 0) {
        (void)prop_variant_clear_at(m, previous);
        co_task_mem_free_guest(previous);
    }
    return com::S_OK;
}

u32 ComCoreManager::shelllink_propstore_commit(Marshaler& m) {
    VITREO_SHELLLINK_PROPS_OR_RETURN(props);
    (void)props;
    return com::S_OK;
}

#undef VITREO_SHELLLINK_PROPS_OR_RETURN

// [MS-SHLLINK] 2.5.7 PropertyStoreDataBlock: BlockSize, 0xA0000009, then
// [MS-PROPSTORE] serialized storages (one per FMTID: StorageSize, "1SPS",
// FMTID, integer-named values, 0) and a 0 terminator. Each value is
// ValueSize, Id, Reserved(0), then a TypedPropertyValue (type, 2 pad bytes,
// data padded to 4). Data layouts per type are the Windows bytes in
// shelllink_propstore_probe*.expected. VT_EMPTY values are omitted.
std::vector<u8> ComCoreManager::shelllink_property_block(
    Marshaler& m, const std::vector<ShellLinkState::Property>& props) {
    // Windows also writes the creating user's SID ({46588AE2-...},4) and the
    // target's volume GUID ({446D16B1-...},104); Vitreo has no volume GUIDs
    // and writes neither (contracts/ole32 note).
    auto put_u16 = [](std::vector<u8>& out, u16 v) {
        out.push_back(static_cast<u8>(v));
        out.push_back(static_cast<u8>(v >> 8));
    };
    auto put_u32 = [](std::vector<u8>& out, u32 v) {
        for (u32 shift = 0; shift < 32; shift += 8) out.push_back(static_cast<u8>(v >> shift));
    };
    auto pad4 = [](std::vector<u8>& out) {
        while (out.size() % 4 != 0) out.push_back(0);
    };
    auto put_utf16 = [&](std::vector<u8>& out, const std::vector<u16>& chars) {
        for (const u16 c : chars) put_u16(out, c);
    };
    auto read_wide = [&](GuestAddr at) {
        std::vector<u16> chars;
        for (u32 i = 0; at != 0 && i < 0x100000; ++i) {
            const u16 c = m.read_u16(at + i * 2).value_or(0);
            if (c == 0) break;
            chars.push_back(c);
        }
        chars.push_back(0);
        return chars;
    };
    static const Guid kStringNamedFmtid{
        0xD5CDD505u, 0x2E9Cu, 0x101Bu, {0x93, 0x97, 0x08, 0x00, 0x2B, 0x2C, 0xF9, 0xAE}};
    const u32 ptr = m.cpu_is_64bit() ? 8u : 4u;

    // Typed value (type + pad + data), or nullopt when not serializable here.
    auto typed_value = [&](GuestAddr pv, std::vector<u8>& out) -> bool {
        const u16 vt = m.read_u16(pv).value_or(0);
        const GuestAddr value = pv + 8;
        put_u16(out, vt);
        put_u16(out, 0);
        switch (vt) {
        case 1:  // VT_NULL
            return true;
        case 16: case 17:  // VT_I1, VT_UI1
            out.push_back(m.read_u8(value).value_or(0));
            pad4(out);
            return true;
        case 2: case 18: case 11:  // VT_I2, VT_UI2, VT_BOOL
            put_u16(out, m.read_u16(value).value_or(0));
            pad4(out);
            return true;
        case 3: case 19: case 22: case 23: case 4: case 10:  // I4 UI4 INT UINT R4 ERROR
            put_u32(out, m.read_u32(value).value_or(0));
            return true;
        case 20: case 21: case 5: case 6: case 7: case 64:  // I8 UI8 R8 CY DATE FILETIME
            put_u32(out, m.read_u32(value).value_or(0));
            put_u32(out, m.read_u32(value + 4).value_or(0));
            return true;
        case 72: {  // VT_CLSID: puuid -> 16 bytes
            const GuestAddr guid = static_cast<GuestAddr>(m.read_ptr(value).value_or(0));
            for (u32 i = 0; i < 16; ++i) out.push_back(guid ? m.read_u8(guid + i).value_or(0) : 0);
            return true;
        }
        case 31: {  // VT_LPWSTR: character count incl. NUL, UTF-16
            const auto chars = read_wide(static_cast<GuestAddr>(m.read_ptr(value).value_or(0)));
            put_u32(out, static_cast<u32>(chars.size()));
            put_utf16(out, chars);
            pad4(out);
            return true;
        }
        case 8: {  // VT_BSTR: byte count incl. NUL, UTF-16
            const GuestAddr bstr = static_cast<GuestAddr>(m.read_ptr(value).value_or(0));
            const u32 bytes = bstr ? m.read_u32(bstr - 4).value_or(0) : 0;
            put_u32(out, bstr ? bytes + 2 : 0);
            for (u32 i = 0; i < bytes; ++i) out.push_back(m.read_u8(bstr + i).value_or(0));
            if (bstr) put_u16(out, 0);
            pad4(out);
            return true;
        }
        case 30: {  // VT_LPSTR: widened to UTF-16, byte count incl. NUL
            const GuestAddr str = static_cast<GuestAddr>(m.read_ptr(value).value_or(0));
            std::string narrow;
            for (u32 i = 0; str != 0 && i < 0x100000; ++i) {
                const u8 c = m.read_u8(str + i).value_or(0);
                if (c == 0) break;
                narrow.push_back(static_cast<char>(c));
            }
            std::vector<u16> chars(narrow.size() * 2 + 4, 0);
            const auto r = codepage::multi_byte_to_wide(
                codepage::kCpAcp, 0, reinterpret_cast<const u8*>(narrow.data()),
                narrow.size(), chars.data(), chars.size());
            chars.resize(r.count);
            chars.push_back(0);
            put_u32(out, static_cast<u32>(chars.size() * 2));
            put_utf16(out, chars);
            pad4(out);
            return true;
        }
        default:
            (void)ptr;
            return false;
        }
    };

    std::vector<Guid> order;
    for (const auto& prop : props) {
        if (std::none_of(order.begin(), order.end(),
                         [&](const Guid& g) { return guid_equal(g, prop.fmtid); })) {
            order.push_back(prop.fmtid);
        }
    }
    std::vector<u8> storages;
    for (const Guid& fmtid : order) {
        if (guid_equal(fmtid, kStringNamedFmtid)) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ole32!ShellLink IPersistFile::Save: string-named property storage (FMTID_SummaryInformation-style names) is not written\n");
            });
            continue;
        }
        std::vector<u8> values;
        for (const auto& prop : props) {
            if (!guid_equal(prop.fmtid, fmtid)) continue;
            const u16 vt = m.read_u16(prop.value).value_or(0);
            if (vt == 0) continue;  // VT_EMPTY is not saved
            std::vector<u8> typed;
            if (!typed_value(prop.value, typed)) {
                static std::once_flag once;
                std::call_once(once, [vt] {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ole32!ShellLink IPersistFile::Save: serializing PROPVARIANT vt=0x%x (value not written)\n", vt);
                });
                continue;
            }
            put_u32(values, static_cast<u32>(4 + 4 + 1 + typed.size()));
            put_u32(values, prop.pid);
            values.push_back(0);
            values.insert(values.end(), typed.begin(), typed.end());
        }
        std::vector<u8> storage;
        put_u32(storage, static_cast<u32>(4 + 4 + 16 + values.size() + 4));
        put_u32(storage, 0x53505331u);  // "1SPS"
        const auto* g = reinterpret_cast<const u8*>(&fmtid);
        storage.insert(storage.end(), g, g + sizeof(Guid));
        storage.insert(storage.end(), values.begin(), values.end());
        put_u32(storage, 0);
        storages.insert(storages.end(), storage.begin(), storage.end());
    }
    if (storages.empty()) return {};
    std::vector<u8> block;
    put_u32(block, static_cast<u32>(4 + 4 + storages.size() + 4));
    put_u32(block, 0xA0000009u);
    block.insert(block.end(), storages.begin(), storages.end());
    put_u32(block, 0);
    return block;
}

} // namespace vitreo::winapi
