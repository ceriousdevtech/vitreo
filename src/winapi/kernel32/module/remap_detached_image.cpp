/// @file module.cpp
/// @brief Implementation of module management APIs (WIN32-033)

#include "module.hpp"
#include "module_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "capsule/vfs/filesystem.hpp"
#include <vitreo/result.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include <unordered_set>
#include <new>

#include <mach/mach.h>
#include <mach/vm_map.h>

#include <vitreo/diagnostics.hpp>

/// @file remap_detached_image.cpp
/// @brief ModuleManager::remap_detached_image
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

MappedImageSnapshot::~MappedImageSnapshot() {
    if (data == nullptr) return;
    if (owns_vm) {
        (void)vm_deallocate(mach_task_self(),
                            reinterpret_cast<vm_address_t>(data), vm_size);
    } else {
        delete[] data;
    }
}

namespace {

/// Copy-on-write remap of `[src, src + size)` into a fresh mapping.
///
/// The snapshot is a second view of the same physical pages until one side
/// writes, so a 200 MB image costs no resident memory at capture time. It only
/// works when the host backing is page-aligned and contiguous, which is why
/// every failure falls through to a plain copy rather than being reported.
const u8* cow_snapshot(const Byte* src, usize size, usize& vm_size_out) {
    // Diagnostic kill switch so a run can be A/B'd against the plain heap copy
    // without a rebuild. Defaults to on.
    static const bool enabled = [] {
        const char* v = std::getenv("VITREO_MODULE_TEMPLATE_COW");
        return v == nullptr || v[0] != '0';
    }();
    if (!enabled) return nullptr;

    const vm_size_t page = static_cast<vm_size_t>(vm_page_size);
    const auto src_addr = reinterpret_cast<vm_address_t>(src);
    if (page == 0 || (src_addr % page) != 0) {
        return nullptr;
    }
    const vm_size_t rounded = (size + page - 1) & ~(page - 1);
    vm_address_t target = 0;
    vm_prot_t cur_prot = VM_PROT_NONE;
    vm_prot_t max_prot = VM_PROT_NONE;
    const kern_return_t kr = vm_remap(
        mach_task_self(), &target, rounded, /*mask=*/0, VM_FLAGS_ANYWHERE,
        mach_task_self(), src_addr, /*copy=*/TRUE, &cur_prot, &max_prot,
        VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        return nullptr;
    }
    // The remap inherits the source protection, which for a mapped image can
    // exclude read (a guest PAGE_NOACCESS page). The snapshot is only ever
    // read, so ask for read here; if even that is refused, drop it.
    if ((cur_prot & VM_PROT_READ) == 0) {
        if ((max_prot & VM_PROT_READ) == 0 ||
            vm_protect(mach_task_self(), target, rounded, /*set_max=*/FALSE,
                       cur_prot | VM_PROT_READ) != KERN_SUCCESS) {
            (void)vm_deallocate(mach_task_self(), target, rounded);
            return nullptr;
        }
    }
    vm_size_out = static_cast<usize>(rounded);
    return reinterpret_cast<const u8*>(target);
}

}  // namespace

void ModuleManager::capture_mapped_image_template(ModuleInfo& info) {
    info.mapped_image_template.reset();
    if (m_memory == nullptr || info.image_end <= info.base_address) {
        return;
    }
    const usize size = static_cast<usize>(info.image_end - info.base_address);
    auto snapshot = std::make_shared<MappedImageSnapshot>();
    snapshot->size = size;

    const Byte* src = m_memory->host_ptr(info.base_address, size);
    if (src != nullptr) {
        usize vm_size = 0;
        if (const u8* cow = cow_snapshot(src, size, vm_size); cow != nullptr) {
            snapshot->data = cow;
            snapshot->vm_size = vm_size;
            snapshot->owns_vm = true;
            info.mapped_image_template = std::move(snapshot);
            return;
        }
    }

    auto* bytes = new (std::nothrow) u8[size];
    if (bytes == nullptr) return;
    snapshot->data = bytes;
    if (src != nullptr) {
        std::memcpy(bytes, src, size);
    } else if (m_memory->read_bytes(info.base_address, bytes, size).is_err()) {
        return;  // snapshot destructor releases the buffer
    }
    info.mapped_image_template = std::move(snapshot);
}

bool ModuleManager::restore_mapped_image_template(ModuleInfo& info) {
    if (m_memory == nullptr || info.mapped_image_template == nullptr ||
        info.mapped_image_template->data == nullptr ||
        info.image_end <= info.base_address) {
        return false;
    }
    const usize size = static_cast<usize>(info.image_end - info.base_address);
    if (info.mapped_image_template->size != size) {
        return false;
    }

    const GuestAddr base = info.base_address;
    const MappedImageSnapshot& tmpl = *info.mapped_image_template;

    // Drop TLS slots that live in this image before rewriting its pages, then
    // re-install them from the restored TLS directory — same sequence as a
    // first-time load after mapping.
    release_dll_tls_slots_in_range(base, info.image_end);

    for (usize off = 0; off < size; ) {
        const usize chunk = std::min(GUEST_PAGE_SIZE - (off % GUEST_PAGE_SIZE),
                                     size - off);
        const GuestAddr addr = base + off;
        const u8* src = tmpl.data + off;

        const Byte* cur = m_memory->host_ptr(addr, chunk);
        if (cur != nullptr && std::memcmp(cur, src, chunk) == 0) {
            off += chunk;
            continue;
        }

        GuestAddr span_base = 0;
        usize span_size = 0;
        MemoryProtection saved = MemoryProtection::None;
        if (!m_memory->query_region(addr, span_base, span_size, saved)) {
            return false;
        }
        bool lifted = false;
        if (!has_flag(saved, MemoryProtection::Write)) {
            if (m_memory->protect_region(
                    addr, chunk, saved | MemoryProtection::Write).is_err()) {
                return false;
            }
            lifted = true;
        }
        Byte* dst = m_memory->host_ptr_mut(addr, chunk);
        if (dst == nullptr) {
            if (lifted) {
                (void)m_memory->protect_region(addr, chunk, saved);
            }
            return false;
        }
        std::memcpy(dst, src, chunk);
        if (lifted &&
            m_memory->protect_region(addr, chunk, saved).is_err()) {
            return false;
        }
        off += chunk;
    }

    if (info.tls_dir_rva != 0) {
        setup_dll_tls(base, info.bitness,
                      static_cast<GuestAddr>(base + info.tls_dir_rva),
                      info.path.c_str());
    }
    info.dllmain_called = false;
    rebuild_image_protection_runs(info);
    return true;
}

bool ModuleManager::remap_detached_image(ModuleInfo& info) {
    // A post-link snapshot is the same guest-visible image a disk remap would
    // produce after IAT/CFG patching, without unmapping RX pages (and the JIT
    // translations keyed to them).
    if (restore_mapped_image_template(info)) {
        return true;
    }

    // Only a real on-disk PE that this manager mapped can be re-mapped: the
    // section descriptors are what unmap_region needs, and the file is the
    // source of the pristine contents.
    if (m_vfs == nullptr || m_pe_loader == nullptr || m_memory == nullptr ||
        info.mapped_sections.empty() || info.path.empty()) {
        return false;
    }

    auto data_res = module_read_all_bytes(*m_vfs, info.path);
    if (data_res.is_err()) {
        fprintf(stderr,
                "[module] re-map of \"%s\" failed: cannot re-read \"%s\"\n",
                info.name.c_str(), info.path.c_str());
        return false;
    }
    const auto& data = data_res.value();

    auto parse_res = pe::parse_pe(std::span<const u8>(data.data(), data.size()));
    if (parse_res.is_err()) {
        fprintf(stderr, "[module] re-map of \"%s\" failed: PE parse error\n",
                info.name.c_str());
        return false;
    }
    const auto& parsed = parse_res.value();

    const GuestAddr base = info.base_address;

    // Drop the stale image first so the loader can claim the same range. Its
    // TLS slots die with it; returning them keeps a repeated probe/load cycle
    // from exhausting the pool.
    release_dll_tls_slots_in_range(base, info.image_end);
    pe::LoadedImage previous{};
    previous.image_base = base;
    previous.header_size = info.mapped_header_size;
    previous.sections = info.mapped_sections;
    m_pe_loader->unload(previous);

    bool dep_failed = false;
    auto load_res = m_pe_loader->load(
        std::span<const u8>(data.data(), data.size()), base,
        make_companion_resolver(parent_windows_dir(info.path), &dep_failed),
        make_pre_import_callback(info.path, info.name));
    if (load_res.is_err() || dep_failed) {
        // The old image is already gone, so the module cannot be handed back.
        // Mark it Failed and let LoadLibrary report ERROR_DLL_INIT_FAILED
        // rather than returning a base that is no longer mapped.
        fprintf(stderr,
                "[module] re-map of \"%s\" failed at 0x%llx (dep_failed=%d)\n",
                info.name.c_str(), static_cast<unsigned long long>(base),
                dep_failed ? 1 : 0);
        info.state = ModuleInfo::State::Failed;
        return false;
    }

    const auto& img = load_res.value();
    info.base_address = img.image_base;
    info.image_end = img.image_end;
    info.entry_point = img.entry_point;
    info.bitness = img.bitness;
    info.mapped_sections = img.sections;
    info.mapped_header_size = img.header_size;
    rebuild_image_protection_runs(info);
    info.exception_rva = parsed.data_directories[pe::DIR_EXCEPTION].VirtualAddress;
    info.exception_size = parsed.data_directories[pe::DIR_EXCEPTION].Size;
    info.tls_dir_rva = 0;
    if (parsed.num_data_directories > pe::DIR_TLS) {
        const auto& tls_dd = parsed.data_directories[pe::DIR_TLS];
        if (tls_dd.Size != 0) info.tls_dir_rva = tls_dd.VirtualAddress;
    }
    if (parsed.num_data_directories > pe::DIR_RESOURCE) {
        const auto& rsrc_dd = parsed.data_directories[pe::DIR_RESOURCE];
        info.resource_rva = rsrc_dd.VirtualAddress;
        info.resource_size = rsrc_dd.Size;
    }

    // Export addresses are image-relative, so they must be recomputed even
    // though the base is unchanged — the fresh parse is the authority.
    auto ex = parse_exports_from_file(
        std::span<const u8>(data.data(), data.size()), parsed, img.image_base);
    info.exports_by_name = std::move(ex.by_name);
    info.exports_by_ordinal = std::move(ex.by_ordinal);
    info.forwarders_by_name = std::move(ex.forwarders_by_name);
    info.forwarders_by_ordinal = std::move(ex.forwarders_by_ordinal);

    patch_guard_cf_load_config(*m_memory, registry_, parsed, img.image_base,
                               info.path.c_str());
    capture_mapped_image_template(info);

    // The previous instance's TLS template is gone with its image; give the
    // fresh one its own slot exactly as a first-time load would.
    if (info.tls_dir_rva != 0) {
        setup_dll_tls(img.image_base, info.bitness,
                      static_cast<GuestAddr>(img.image_base + info.tls_dir_rva),
                      info.path.c_str());
    }

    info.dllmain_called = false;
    return true;
}

} // namespace vitreo::winapi
