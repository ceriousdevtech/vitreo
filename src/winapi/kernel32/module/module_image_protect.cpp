// SPDX-License-Identifier: LGPL-2.1-only

/// @file module_image_protect.cpp
/// @brief MEM_IMAGE Win32 PAGE_*WRITECOPY tracking for VirtualProtect/Query.

#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/memory/memory.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace vitreo::winapi {
namespace {

constexpr u32 kPageMask = 0xFFu;

[[nodiscard]] u32 page_base_prot(u32 win32_prot) noexcept {
    return win32_prot & kPageMask;
}

/// WRITECOPY forms become private RW/ERW once the page differs from the
/// post-link image template (a private write broke COW).
[[nodiscard]] u32 report_image_protect(const ModuleInfo& info,
                                       GuestMemory* memory,
                                       u64 page_offset,
                                       u32 stored) noexcept {
    const u32 base = page_base_prot(stored);
    if (base != PAGE_WRITECOPY && base != PAGE_EXECUTE_WRITECOPY) {
        return stored;
    }
    if (memory == nullptr || info.mapped_image_template == nullptr ||
        info.mapped_image_template->data == nullptr) {
        return stored;
    }
    const usize tmpl_size = info.mapped_image_template->size;
    if (page_offset >= tmpl_size) return stored;
    const usize len = std::min<usize>(GUEST_PAGE_SIZE, tmpl_size - page_offset);
    if (len == 0) return stored;
    const Byte* cur =
        memory->host_ptr(info.base_address + page_offset, len);
    if (cur == nullptr) return stored;
    if (std::memcmp(cur, info.mapped_image_template->data + page_offset,
                    len) == 0) {
        return stored;
    }
    if (base == PAGE_WRITECOPY) {
        return (stored & ~kPageMask) | PAGE_READWRITE;
    }
    return (stored & ~kPageMask) | PAGE_EXECUTE_READWRITE;
}

[[nodiscard]] ModuleInfo* module_info_for_address(
    std::unordered_map<std::string, ModuleInfo>& modules,
    GuestAddr addr) {
    if (addr == 0) return nullptr;
    for (auto& [name, info] : modules) {
        (void)name;
        if (info.state != ModuleInfo::State::Loaded || info.ref_count == 0) {
            continue;
        }
        GuestAddr end = info.image_end;
        if (end == 0) {
            if (!info.synthetic_image) continue;
            end = info.base_address + SYNTHETIC_MODULE_SLOT_SIZE;
        }
        if (addr >= info.base_address && addr < end) {
            return &info;
        }
    }
    return nullptr;
}

}  // namespace

void ModuleManager::rebuild_image_protection_runs(ModuleInfo& info) const {
    info.protection_runs.clear();
    if (info.image_end <= info.base_address || info.mapped_sections.empty()) {
        return;
    }
    // PE headers are PAGE_READONLY until VirtualProtect changes them.
    info.protection_runs.emplace(0, PAGE_READONLY);
    for (const auto& sec : info.mapped_sections) {
        if (sec.size == 0 || sec.guest_address < info.base_address) continue;
        const u64 off = static_cast<u64>(sec.guest_address - info.base_address);
        if (off >= static_cast<u64>(info.image_end - info.base_address)) {
            continue;
        }
        u32 wp = sec.win32_protection;
        if (wp == 0) {
            wp = native_prot_to_win32(sec.protection);
        }
        info.protection_runs[off] = wp;
    }
}

void ModuleManager::set_mapped_image_layout(
    GuestAddr base, std::vector<pe::MappedSection> sections,
    usize header_size) {
    std::shared_lock<HostRwLock> tables(m_tables_mutex);
    auto it = base_to_name_.find(base);
    if (it == base_to_name_.end()) return;
    auto mod = modules_by_name_.find(it->second);
    if (mod == modules_by_name_.end()) return;
    mod->second.mapped_sections = std::move(sections);
    mod->second.mapped_header_size = header_size;
    rebuild_image_protection_runs(mod->second);
}

bool ModuleManager::query_image_protection(
    GuestAddr address, GuestAddr& image_base, u64& image_size,
    u32* protection, u64* protection_run_end,
    u32* allocation_protection) const {
    const auto at = module_for_address(address);
    if (at.info == nullptr || at.info->protection_runs.empty()) {
        return false;
    }
    const ModuleInfo& info = *at.info;
    image_base = info.base_address;
    image_size = static_cast<u64>(info.image_end - info.base_address);
    const u64 page_off = align_down(static_cast<u64>(address - info.base_address),
                                    static_cast<u64>(GUEST_PAGE_SIZE));
    const auto run =
        std::prev(info.protection_runs.upper_bound(page_off));
    const u32 stored = run->second;
    const u32 reported =
        report_image_protect(info, m_memory, page_off, stored);
    if (protection != nullptr) *protection = reported;
    if (allocation_protection != nullptr) {
        *allocation_protection = ModuleInfo::kImageAllocationProtect;
    }
    if (protection_run_end != nullptr) {
        // RegionSize spans every following page whose reported protect is
        // the same, across section boundaries (writecopy_protect_probe
        // "img ro q" covers .cowro and the read-only sections after it), and
        // stops at a dirty COW page inside a WRITECOPY run.
        u64 end_off = image_size;
        for (auto r = run; r != info.protection_runs.end(); ++r) {
            const auto next = std::next(r);
            const u64 r_end = next == info.protection_runs.end()
                                  ? image_size
                                  : next->first;
            const u32 base = page_base_prot(r->second);
            const bool per_page =
                base == PAGE_WRITECOPY || base == PAGE_EXECUTE_WRITECOPY;
            u64 off = r == run ? page_off + GUEST_PAGE_SIZE : r->first;
            if (!per_page) {
                if (off < r_end && r->second != reported) {
                    end_off = off;
                    break;
                }
                continue;
            }
            for (; off < r_end; off += GUEST_PAGE_SIZE) {
                if (report_image_protect(info, m_memory, off, r->second) !=
                    reported) {
                    break;
                }
            }
            if (off < r_end) {
                end_off = off;
                break;
            }
        }
        *protection_run_end = info.base_address + end_off;
    }
    return true;
}

int ModuleManager::virtual_protect_image(GuestAddr address, usize size,
                                         u32 new_protect, u32* old_protect) {
    if (m_memory == nullptr) return 0;
    ModuleInfo* info = nullptr;
    {
        std::shared_lock<HostRwLock> tables(m_tables_mutex);
        info = module_info_for_address(
            modules_by_name_,
            align_down(address, static_cast<GuestAddr>(GUEST_PAGE_SIZE)));
    }
    if (info == nullptr || info->protection_runs.empty()) {
        return 0;  // NotOwned
    }
    // MEM_IMAGE pages accept the WRITECOPY forms on every section, including
    // read-only and code pages (writecopy_protect_probe img ... wc / ewc).
    if (size == 0 || address > std::numeric_limits<u64>::max() - size ||
        !is_valid_protect_request(new_protect)) {
        return -2;
    }
    const GuestAddr page_base =
        align_down(address, static_cast<GuestAddr>(GUEST_PAGE_SIZE));
    const GuestAddr page_end =
        align_up(address + size, static_cast<GuestAddr>(GUEST_PAGE_SIZE));
    if (page_end > info->image_end || page_base < info->base_address) {
        return -1;
    }

    const u64 begin_off = page_base - info->base_address;
    const u64 end_off = page_end - info->base_address;
    const u64 image_size =
        static_cast<u64>(info->image_end - info->base_address);
    const auto run =
        std::prev(info->protection_runs.upper_bound(begin_off));
    const u32 old_stored = run->second;
    if (old_protect != nullptr) {
        *old_protect =
            report_image_protect(*info, m_memory, begin_off, old_stored);
    }

    const u32 stored = stored_image_win32_protection(new_protect);
    const MemoryProtection native = win32_prot_to_native(stored);
    if (m_memory
            ->protect_region(page_base,
                             static_cast<usize>(page_end - page_base), native)
            .is_err()) {
        return -1;
    }

    const u32 after = end_off < image_size
        ? std::prev(info->protection_runs.upper_bound(end_off))->second
        : 0u;
    auto erase_begin = info->protection_runs.lower_bound(begin_off);
    auto erase_end = info->protection_runs.lower_bound(end_off);
    info->protection_runs.erase(erase_begin, erase_end);
    info->protection_runs[begin_off] = stored;
    if (end_off < image_size) {
        info->protection_runs[end_off] = after;
    }
    auto inserted = info->protection_runs.find(begin_off);
    if (inserted != info->protection_runs.begin()) {
        auto previous = std::prev(inserted);
        if (previous->second == inserted->second) {
            info->protection_runs.erase(inserted);
        }
    }
    auto boundary = info->protection_runs.find(end_off);
    if (boundary != info->protection_runs.end() &&
        boundary != info->protection_runs.begin()) {
        auto previous = std::prev(boundary);
        if (previous->second == boundary->second) {
            info->protection_runs.erase(boundary);
        }
    }

    if (is_execute_win32_protection(stored)) {
        m_memory->notify_executable_write(
            page_base, static_cast<usize>(page_end - page_base));
    }
    return 1;
}

}  // namespace vitreo::winapi
