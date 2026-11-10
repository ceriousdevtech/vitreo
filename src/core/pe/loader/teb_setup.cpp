// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "teb_setup.hpp"
#include "core/pe/os_version.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/cpu/state/cpu_state.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <initializer_list>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace vitreo::pe {

namespace {

constexpr u64 LDR_PAGE_OFFSET = 0x2000;
constexpr u64 LDR_PAGE_SIZE = 0x1000;
constexpr u64 ENTRY_PAGE_SIZE = 0x1000;
// One page per module: LDR_DATA_TABLE_ENTRY at +0, its LDR_DDAG_NODE at
// +DDAG_NODE_OFFSET, the UTF-16 path past both. The name must not overlap the
// Win10+ tail of the entry (OriginalBase .. HotPatchState).
constexpr u64 ENTRY_DDAG_NODE_OFFSET_64 = 0x140;
constexpr u64 ENTRY_DDAG_NODE_OFFSET_32 = 0xC0;
constexpr u64 ENTRY_NAME_OFFSET = 0x200;
constexpr u64 ENTRY_SEARCH_BASE = 0x10000;
// ntdll keeps LdrpHashTable (32 LIST_ENTRY buckets keyed by
// RtlHashUnicodeString(BaseDllName, TRUE, X65599) & 31) outside every entry;
// Vitreo keeps it in the loader page after PEB_LDR_DATA.
constexpr u64 LDR_HASH_TABLE_OFFSET = 0x100;
constexpr u32 LDR_HASH_BUCKETS = 32;

/// LDR_DATA_TABLE_ENTRY and LDR_DDAG_NODE field offsets, Windows 10+.
/// Measured: tools/oracle_probe/ldr_entry_contract_probe (entry heap size
/// 0x138 / 0xB8; TimeDateStamp, CheckSum, OriginalBase and BaseNameHashValue
/// each verified against its source).
struct LdrEntryLayout {
    u64 ptr;
    u64 dll_base, entry_point, size_of_image, full_name, base_name;
    u64 flags, obsolete_load_count, tls_index, hash_links, time_date_stamp;
    u64 ddag_node, node_module_link, original_base, load_time;
    u64 base_name_hash, load_reason, reference_count, dependent_load_flags;
    u64 check_sum;
    u64 ddag_offset;  // where this page keeps the entry's LDR_DDAG_NODE
    u64 ddag_load_count, ddag_state;
};
constexpr LdrEntryLayout kLdrEntry64{
    8,    0x30, 0x38, 0x40, 0x48, 0x58,
    0x68, 0x6C, 0x6E, 0x70, 0x80,
    0x98, 0xA0, 0xF8, 0x100,
    0x108, 0x10C, 0x114, 0x118,
    0x120,
    ENTRY_DDAG_NODE_OFFSET_64,
    0x18, 0x38};
constexpr LdrEntryLayout kLdrEntry32{
    4,    0x18, 0x1C, 0x20, 0x24, 0x2C,
    0x34, 0x38, 0x3A, 0x3C, 0x44,
    0x50, 0x54, 0x80, 0x88,
    0x90, 0x94, 0x9C, 0xA0,
    0xA8,
    ENTRY_DDAG_NODE_OFFSET_32,
    0x0C, 0x20};

// LDR_DATA_TABLE_ENTRY.Flags bits, composed exactly as measured
// (ldr_entry_contract_probe): exe 0x22CC; ntdll 0x20A2C4 / 0xA2C4; attached
// DLL 0x28A2CC / 0x8A2CC (+DontCallForThreads for kernel32). ReservedFlags1's
// high bit (0x800) was observed both set and clear for the same modules across
// runs on one machine; it is left clear.
constexpr u32 kLdrImageDll = 0x00000004;
constexpr u32 kLdrLoadNotificationsSent = 0x00000008;
constexpr u32 kLdrInLegacyLists = 0x00000040;
constexpr u32 kLdrInIndexes = 0x00000080;
constexpr u32 kLdrInExceptionTable = 0x00000200;
constexpr u32 kLdrLoadConfigProcessed = 0x00002000;
constexpr u32 kLdrProtectDelayLoad = 0x00008000;    // every DLL, never the image
constexpr u32 kLdrDontCallForThreads = 0x00040000;
constexpr u32 kLdrProcessAttachCalled = 0x00080000;
constexpr u32 kLdrX64DllBit21 = 0x00200000;         // every x64 DLL, no x86 one
// LDR_DLL_LOAD_REASON and LDR_DDAG_STATE values.
constexpr u32 kLoadReasonStaticDependency = 0;
constexpr u32 kLoadReasonDynamicLoad = 4;
constexpr u32 kDdagStateReadyToRun = 9;

/// The PE header fields an entry mirrors, read from the mapped image. A
/// synthetic (thunk-backed) module maps no header, so every field reads 0.
struct MappedImageFacts {
    u32 time_date_stamp{0};
    u32 check_sum{0};
    bool has_tls{false};
    u16 dependent_load_flags{0};
};

MappedImageFacts read_mapped_image_facts(GuestMemory& memory, u64 base) {
    MappedImageFacts facts;
    if (memory.read_u16(base).value_or(0) != 0x5A4D) return facts;  // "MZ"
    const u64 nt = base + memory.read_u32(base + 0x3C).value_or(0);
    if (memory.read_u32(nt).value_or(0) != 0x00004550) return facts;  // "PE\0\0"
    facts.time_date_stamp = memory.read_u32(nt + 0x08).value_or(0);
    const u64 opt = nt + 0x18;
    const bool pe32plus = memory.read_u16(opt).value_or(0) == 0x20B;
    facts.check_sum = memory.read_u32(opt + 0x40).value_or(0);
    const u32 dir_count = memory.read_u32(opt + (pe32plus ? 0x6C : 0x5C)).value_or(0);
    const u64 dirs = opt + (pe32plus ? 0x70 : 0x60);
    constexpr u32 kTlsDirectory = 9;
    constexpr u32 kLoadConfigDirectory = 10;
    if (dir_count > kTlsDirectory) {
        facts.has_tls = memory.read_u32(dirs + kTlsDirectory * 8 + 4).value_or(0) != 0;
    }
    if (dir_count > kLoadConfigDirectory) {
        const u32 rva = memory.read_u32(dirs + kLoadConfigDirectory * 8).value_or(0);
        // IMAGE_LOAD_CONFIG_DIRECTORY{64,32}.DependentLoadFlags, present when
        // the directory's own Size field reaches it.
        const u64 field = pe32plus ? 0x4E : 0x36;
        if (rva != 0 && memory.read_u32(base + rva).value_or(0) >= field + 2) {
            facts.dependent_load_flags = memory.read_u16(base + rva + field).value_or(0);
        }
    }
    return facts;
}

/// RtlUpcaseUnicodeChar over the code units this loader writes (paths are
/// stored one byte per code unit, so only U+0000..U+00FF occur).
u16 upcase_unit(u16 c) {
    if (c >= 'a' && c <= 'z') return static_cast<u16>(c - 0x20);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return static_cast<u16>(c - 0x20);
    if (c == 0xFF) return 0x178;
    if (c == 0xB5) return 0x39C;
    return c;
}

/// RtlHashUnicodeString(name, CaseInSensitive=TRUE, HASH_STRING_ALGORITHM_X65599).
u32 hash_base_name(std::string_view base_name) {
    u32 h = 0;
    for (const char c : base_name) {
        h = h * 65599u + upcase_unit(static_cast<u8>(c));
    }
    return h;
}

std::string_view base_name_of(std::string_view path) {
    const usize separator = path.find_last_of("\\/");
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

bool equals_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

u32 ldr_entry_flags(const TebModuleInfo& module, bool is_64bit) {
    const bool is_ntdll = equals_ignore_case(base_name_of(module.path), "ntdll.dll");
    u32 flags = kLdrImageDll | kLdrInLegacyLists | kLdrInIndexes |
                kLdrInExceptionTable | kLdrLoadConfigProcessed;
    // ntdll is mapped by the kernel before any notification can be sent.
    if (!is_ntdll) flags |= kLdrLoadNotificationsSent;
    if (!module.is_executable) {
        flags |= kLdrProtectDelayLoad;
        if (is_64bit) flags |= kLdrX64DllBit21;
        if (!is_ntdll && module.process_attach_called) flags |= kLdrProcessAttachCalled;
        if (module.thread_calls_disabled) flags |= kLdrDontCallForThreads;
    }
    return flags;
}

/// Windows FILETIME (100 ns since 1601-01-01 UTC) for LoadTime.
u64 current_filetime() {
    constexpr u64 kUnixEpochAsFiletime = 116444736000000000ULL;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return kUnixEpochAsFiletime + static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100);
}

u16 write_ascii_as_utf16(GuestMemory& memory, u64 buf,
                         std::string_view text) noexcept {
    for (usize i = 0; i < text.size(); ++i) {
        (void)memory.write_u16(buf + i * 2, static_cast<u8>(text[i]));
    }
    (void)memory.write_u16(buf + text.size() * 2, 0);
    const usize bytes = text.size() * 2;
    return static_cast<u16>(bytes > 0xFFFEu ? 0xFFFEu : bytes);
}

void write_unicode_string64(GuestMemory& memory, u64 us, u64 buf,
                            u16 byte_len) noexcept {
    (void)memory.write_u16(us + 0, byte_len);
    (void)memory.write_u16(us + 2, static_cast<u16>(byte_len + 2));
    (void)memory.write_u32(us + 4, 0);
    (void)memory.write_u64(us + 8, buf);
}

void write_unicode_string32(GuestMemory& memory, u64 us, u64 buf,
                            u16 byte_len) noexcept {
    (void)memory.write_u16(us + 0, byte_len);
    (void)memory.write_u16(us + 2, static_cast<u16>(byte_len + 2));
    (void)memory.write_u32(us + 4, static_cast<u32>(buf));
}

namespace {

/// Rewrite the InInitializationOrder list to hold exactly @p order's modules
/// in that order. Entries are found by DllBase on the load-order list.
void relink_initialization_order(GuestMemory& memory, u64 peb_address, bool is_64bit,
                                 const std::vector<TebModuleInfo>& order) {
    const auto read_ptr = [&](u64 at) -> u64 {
        return is_64bit ? memory.read_u64(at).value_or(0)
                        : memory.read_u32(at).value_or(0);
    };
    const auto write_ptr = [&](u64 at, u64 value) {
        if (is_64bit) (void)memory.write_u64(at, value);
        else (void)memory.write_u32(at, static_cast<u32>(value));
    };
    const u64 ldr = read_ptr(peb_address + (is_64bit ? 0x18 : 0x0C));
    if (ldr == 0) return;
    const u64 load_head = ldr + (is_64bit ? 0x10 : 0x0C);
    const u64 init_head = ldr + (is_64bit ? 0x30 : 0x1C);
    const u64 init_links = is_64bit ? 0x20 : 0x10;
    const u64 dll_base = is_64bit ? 0x30 : 0x18;
    const u64 ptr = is_64bit ? 8 : 4;

    std::vector<u64> links;
    for (const auto& module : order) {
        u64 entry = read_ptr(load_head);
        for (usize guard = 0; entry != load_head && entry != 0 && guard < 4096; ++guard) {
            if (read_ptr(entry + dll_base) == module.base_address) {
                links.push_back(entry + init_links);
                break;
            }
            entry = read_ptr(entry);
        }
    }
    u64 previous = init_head;
    for (const u64 link : links) {
        write_ptr(previous, link);           // previous->Flink
        write_ptr(link + ptr, previous);     // link->Blink
        previous = link;
    }
    write_ptr(previous, init_head);
    write_ptr(init_head + ptr, previous);
}

}  // namespace

bool setup_peb_loader_data(GuestMemory& memory, u64 peb_address,
                           bool is_64bit, u64 image_base,
                           std::string_view image_path,
                           const std::vector<TebModuleInfo>& loaded_modules) {
    const u64 ldr = peb_address + LDR_PAGE_OFFSET;
    if (memory.map_region(ldr, LDR_PAGE_SIZE,
                          MemoryProtection::ReadWrite).is_err()) {
        return false;
    }

    std::vector<TebModuleInfo> modules = loaded_modules;
    if (modules.empty()) {
        TebModuleInfo self{
            image_base,
            image_path.empty() ? std::string("C:\\vitreo.exe")
                               : std::string(image_path)};
        self.is_executable = true;
        modules.push_back(self);
    }
    if (is_64bit) {
        // The public winternl PEB layout places Ldr at +0x18. PEB_LDR_DATA
        // carries three list heads -- InLoadOrder at +0x10, InMemoryOrder at
        // +0x20, InInitializationOrder at +0x30 -- and every one of them must
        // be a valid (initially self-linked) LIST_ENTRY: a guest that walks a
        // list whose Flink is still zero reads through a null pointer.
        (void)memory.write_u64(peb_address + 0x18, ldr);
        (void)memory.write_u32(ldr + 0x00, 0x58); // Length
        (void)memory.write_u8(ldr + 0x04, 1);     // Initialized
        for (const u64 head : {ldr + 0x10, ldr + 0x20, ldr + 0x30}) {
            (void)memory.write_u64(head + 0x00, head);
            (void)memory.write_u64(head + 0x08, head);
        }
        for (u32 bucket = 0; bucket < LDR_HASH_BUCKETS; ++bucket) {
            const u64 head = ldr + LDR_HASH_TABLE_OFFSET + bucket * 0x10;
            (void)memory.write_u64(head + 0x00, head);
            (void)memory.write_u64(head + 0x08, head);
        }
    } else {
        (void)memory.write_u32(peb_address + 0x0C,
                               static_cast<u32>(ldr));
        (void)memory.write_u32(ldr + 0x00, 0x30); // Length
        (void)memory.write_u8(ldr + 0x04, 1);     // Initialized
        for (const u64 head : {ldr + 0x0C, ldr + 0x14, ldr + 0x1C}) {
            (void)memory.write_u32(head + 0x00, static_cast<u32>(head));
            (void)memory.write_u32(head + 0x04, static_cast<u32>(head));
        }
        for (u32 bucket = 0; bucket < LDR_HASH_BUCKETS; ++bucket) {
            const u64 head = ldr + LDR_HASH_TABLE_OFFSET + bucket * 0x08;
            (void)memory.write_u32(head + 0x00, static_cast<u32>(head));
            (void)memory.write_u32(head + 0x04, static_cast<u32>(head));
        }
    }

    // Windows starts every process's lists with the image, then ntdll,
    // kernel32 and kernelbase; the initialization-order list starts ntdll,
    // kernelbase, kernel32 (tools/oracle_probe/loader_order_probe, x64 and
    // x86). Code that takes "the second load-order entry is ntdll" relies on
    // it, so the startup set is written in that order, the rest unchanged.
    const auto basename_rank = [](const TebModuleInfo& module,
                                  std::initializer_list<std::string_view> names) {
        const usize separator = module.path.find_last_of("\\/");
        std::string base = module.path.substr(
            separator == std::string::npos ? 0 : separator + 1);
        for (char& c : base) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        usize rank = 0;
        for (const std::string_view name : names) {
            if (base == name) return rank;
            ++rank;
        }
        return names.size();
    };
    std::stable_sort(modules.begin(), modules.end(),
                     [&](const TebModuleInfo& a, const TebModuleInfo& b) {
        if (a.is_executable != b.is_executable) return a.is_executable;
        return basename_rank(a, {"ntdll.dll", "kernel32.dll", "kernelbase.dll"}) <
               basename_rank(b, {"ntdll.dll", "kernel32.dll", "kernelbase.dll"});
    });

    for (const auto& module : modules) {
        if (!append_peb_loader_module(memory, peb_address, is_64bit, module)) {
            return false;
        }
    }

    // Relink the initialization-order list: ntdll, kernelbase, kernel32,
    // then the others in load order (the image is not on this list).
    std::vector<TebModuleInfo> init_order;
    for (const auto& module : modules) {
        if (!module.is_executable) init_order.push_back(module);
    }
    std::stable_sort(init_order.begin(), init_order.end(),
                     [&](const TebModuleInfo& a, const TebModuleInfo& b) {
        return basename_rank(a, {"ntdll.dll", "kernelbase.dll", "kernel32.dll"}) <
               basename_rank(b, {"ntdll.dll", "kernelbase.dll", "kernel32.dll"});
    });
    relink_initialization_order(memory, peb_address, is_64bit, init_order);

    return true;
}

}  // namespace

namespace {

u64 read_guest_ptr(GuestMemory& memory, bool is_64bit, u64 at) {
    return is_64bit ? memory.read_u64(at).value_or(0)
                    : memory.read_u32(at).value_or(0);
}

void write_guest_ptr(GuestMemory& memory, bool is_64bit, u64 at, u64 value) {
    if (is_64bit) (void)memory.write_u64(at, value);
    else (void)memory.write_u32(at, static_cast<u32>(value));
}

/// Insert @p link at the tail of the LIST_ENTRY ring headed by @p head.
void list_insert_tail(GuestMemory& memory, bool is_64bit, u64 head, u64 link) {
    const u64 ptr = is_64bit ? 8 : 4;
    if (read_guest_ptr(memory, is_64bit, head) == 0) {  // never initialized
        write_guest_ptr(memory, is_64bit, head, head);
        write_guest_ptr(memory, is_64bit, head + ptr, head);
    }
    const u64 tail = read_guest_ptr(memory, is_64bit, head + ptr);
    write_guest_ptr(memory, is_64bit, link, head);
    write_guest_ptr(memory, is_64bit, link + ptr, tail);
    write_guest_ptr(memory, is_64bit, tail, link);
    write_guest_ptr(memory, is_64bit, head + ptr, link);
}

/// The entry whose DllBase is @p base on the load-order list, or 0.
u64 find_ldr_entry(GuestMemory& memory, u64 peb_address, bool is_64bit, u64 base) {
    const LdrEntryLayout& layout = is_64bit ? kLdrEntry64 : kLdrEntry32;
    const u64 ldr = read_guest_ptr(memory, is_64bit, peb_address + (is_64bit ? 0x18 : 0x0C));
    if (ldr == 0) return 0;
    const u64 head = ldr + (is_64bit ? 0x10 : 0x0C);
    u64 link = read_guest_ptr(memory, is_64bit, head);
    for (usize guard = 0; link != head && link != 0 && guard < 65536; ++guard) {
        if (read_guest_ptr(memory, is_64bit, link + layout.dll_base) == base) return link;
        link = read_guest_ptr(memory, is_64bit, link);
    }
    return 0;
}

/// The state-dependent fields: Flags and DdagNode->LoadCount.
void write_ldr_entry_state(GuestMemory& memory, bool is_64bit, u64 entry,
                           const TebModuleInfo& module) {
    const LdrEntryLayout& layout = is_64bit ? kLdrEntry64 : kLdrEntry32;
    (void)memory.write_u32(entry + layout.flags, ldr_entry_flags(module, is_64bit));
    // A module of the startup set is pinned: LoadCount -1 (measured).
    (void)memory.write_u32(entry + layout.ddag_offset + layout.ddag_load_count,
                           module.dynamic_load ? module.load_count : 0xFFFFFFFFu);
}

/// Every Win10+ field past the names. Fields the probe measured as zero for
/// all modules (EntryPointActivationContext, Lock, LoadContext, ActivePatch-
/// ImageBase, HotPatchState, SigningLevel) stay zero from the fresh page.
/// Not modelled (recorded in .docs/contracts/ldr_data_table_entry.md):
/// ParentDllBase, SwitchBackContext, BaseAddressIndexNode /
/// MappingInfoIndexNode (ntdll's red-black indexes), ImplicitPathOptions and
/// the DdagNode dependency graph.
void write_ldr_entry_tail(GuestMemory& memory, bool is_64bit, u64 ldr, u64 entry,
                          const TebModuleInfo& module, std::string_view base_name) {
    const LdrEntryLayout& layout = is_64bit ? kLdrEntry64 : kLdrEntry32;
    const MappedImageFacts facts = read_mapped_image_facts(memory, module.base_address);
    const bool is_ntdll = equals_ignore_case(base_name, "ntdll.dll");
    const bool is_kernel32 = equals_ignore_case(base_name, "kernel32.dll");

    (void)memory.write_u16(entry + layout.obsolete_load_count,
                           module.dynamic_load ? 6 : 0xFFFF);
    (void)memory.write_u16(entry + layout.tls_index, facts.has_tls ? 0xFFFF : 0);
    (void)memory.write_u32(entry + layout.time_date_stamp, facts.time_date_stamp);
    write_guest_ptr(memory, is_64bit, entry + layout.original_base, module.base_address);
    (void)memory.write_u64(entry + layout.load_time, current_filetime());
    const u32 hash = hash_base_name(base_name);
    (void)memory.write_u32(entry + layout.base_name_hash, hash);
    // The loader loads the image and kernel32 itself (DynamicLoad); every
    // other startup module is a static dependency.
    (void)memory.write_u32(entry + layout.load_reason,
                           module.dynamic_load || module.is_executable || is_kernel32
                               ? kLoadReasonDynamicLoad
                               : kLoadReasonStaticDependency);
    (void)memory.write_u32(entry + layout.reference_count,
                           !module.dynamic_load &&
                                   (module.is_executable || is_ntdll || is_kernel32)
                               ? 2u : 1u);
    (void)memory.write_u32(entry + layout.dependent_load_flags, facts.dependent_load_flags);
    (void)memory.write_u32(entry + layout.check_sum, facts.check_sum);

    const u64 bucket = hash % LDR_HASH_BUCKETS;
    list_insert_tail(memory, is_64bit,
                     ldr + LDR_HASH_TABLE_OFFSET + bucket * (is_64bit ? 0x10 : 0x08),
                     entry + layout.hash_links);

    // LDR_DDAG_NODE: Modules ring holding this entry's NodeModuleLink,
    // State LdrModulesReadyToRun.
    const u64 node = entry + layout.ddag_offset;
    write_guest_ptr(memory, is_64bit, node, node);
    write_guest_ptr(memory, is_64bit, node + layout.ptr, node);
    list_insert_tail(memory, is_64bit, node, entry + layout.node_module_link);
    (void)memory.write_u32(node + layout.ddag_state, kDdagStateReadyToRun);
    write_guest_ptr(memory, is_64bit, entry + layout.ddag_node, node);

    write_ldr_entry_state(memory, is_64bit, entry, module);
}

}  // namespace

bool update_peb_loader_module(GuestMemory& memory, u64 peb_address,
                              bool is_64bit, const TebModuleInfo& module) {
    if (peb_address == 0 || module.base_address == 0) return false;
    const u64 entry = find_ldr_entry(memory, peb_address, is_64bit, module.base_address);
    if (entry == 0) return false;
    // Whether the entry was published as a dynamic load is fixed at
    // publication; its ObsoleteLoadCount (6 vs 0xFFFF) records it.
    const LdrEntryLayout& layout = is_64bit ? kLdrEntry64 : kLdrEntry32;
    TebModuleInfo state = module;
    state.dynamic_load =
        memory.read_u16(entry + layout.obsolete_load_count).value_or(0xFFFF) == 6;
    write_ldr_entry_state(memory, is_64bit, entry, state);
    return true;
}

bool append_peb_loader_module(GuestMemory& memory, u64 peb_address,
                              bool is_64bit,
                              const TebModuleInfo& module) {
    if (peb_address == 0 || module.base_address == 0) return false;

    const auto ldr_result = is_64bit
        ? memory.read_u64(peb_address + 0x18)
        : memory.read_u32(peb_address + 0x0C).map([](u32 value) {
              return static_cast<u64>(value);
          });
    if (ldr_result.is_err() || ldr_result.value() == 0) return false;

    const u64 ldr = ldr_result.value();
    const u64 head = ldr + (is_64bit ? 0x20 : 0x14);
    const u64 link_offset = is_64bit ? 0x10 : 0x08;
    std::unordered_set<u64> visited;
    u64 links = is_64bit ? memory.read_u64(head).value_or(0)
                         : memory.read_u32(head).map([](u32 value) {
                               return static_cast<u64>(value);
                           }).value_or(0);
    while (links != head) {
        if (links == 0 || !visited.insert(links).second || links < link_offset) {
            return false;
        }
        const u64 entry = links - link_offset;
        const auto base_result = is_64bit
            ? memory.read_u64(entry + 0x30)
            : memory.read_u32(entry + 0x18).map([](u32 value) {
                  return static_cast<u64>(value);
              });
        if (base_result.is_err()) return false;
        if (base_result.value() == module.base_address) return true;
        const auto next = is_64bit
            ? memory.read_u64(links)
            : memory.read_u32(links).map([](u32 value) {
                  return static_cast<u64>(value);
              });
        if (next.is_err()) return false;
        links = next.value();
    }

    const u64 entry = memory.find_free_gap(ENTRY_SEARCH_BASE, ENTRY_PAGE_SIZE);
    if (entry == 0 || (!is_64bit && entry > 0xFFFFFFFFULL) ||
        memory.map_region(entry, ENTRY_PAGE_SIZE,
                          MemoryProtection::ReadWrite).is_err()) {
        return false;
    }
    links = entry + link_offset;
    const u64 name = entry + ENTRY_NAME_OFFSET;
    const std::string path = module.path.substr(
        0, (ENTRY_PAGE_SIZE - ENTRY_NAME_OFFSET) / 2 - 1);
    const u16 name_bytes = write_ascii_as_utf16(memory, name, path);
    const usize separator = path.find_last_of("\\/");
    const usize base_offset = separator == std::string::npos ? 0 : separator + 1;
    const u64 base_name = name + base_offset * 2;
    const u16 base_name_bytes = static_cast<u16>(
        name_bytes - static_cast<u16>(base_offset * 2));

    if (is_64bit) {
        // Insert at the tail of each of the three loader lists. The links live
        // at InLoadOrderLinks +0x00, InMemoryOrderLinks +0x10 and
        // InInitializationOrderLinks +0x20 of the entry, matching the three
        // PEB_LDR_DATA heads.
        const u64 list_heads[3] = {ldr + 0x10, ldr + 0x20, ldr + 0x30};
        const u64 list_links[3] = {entry + 0x00, entry + 0x10, entry + 0x20};
        const usize list_count = module.is_executable ? 2 : 3;
        for (usize i = 0; i < list_count; ++i) {
            const auto tail_result = memory.read_u64(list_heads[i] + 0x08);
            if (tail_result.is_err()) return false;
            const u64 tail = tail_result.value();
            (void)memory.write_u64(list_links[i] + 0x00, list_heads[i]);
            (void)memory.write_u64(list_links[i] + 0x08, tail);
            (void)memory.write_u64(tail + 0x00, list_links[i]);
            (void)memory.write_u64(list_heads[i] + 0x08, list_links[i]);
        }
        (void)memory.write_u64(entry + 0x30, module.base_address);
        (void)memory.write_u64(entry + 0x38, module.entry_point);
        (void)memory.write_u32(entry + 0x40,
                               static_cast<u32>(module.size_of_image));
        write_unicode_string64(memory, entry + 0x48, name, name_bytes);
        write_unicode_string64(memory, entry + 0x58, base_name,
                               base_name_bytes);
        write_ldr_entry_tail(memory, true, ldr, entry, module,
                             std::string_view(path).substr(base_offset));
    } else {
        if (module.base_address > 0xFFFFFFFFULL) {
            (void)memory.unmap_region(entry, ENTRY_PAGE_SIZE);
            return false;
        }
        const u64 list_heads[3] = {ldr + 0x0C, ldr + 0x14, ldr + 0x1C};
        const u64 list_links[3] = {entry + 0x00, entry + 0x08, entry + 0x10};
        const usize list_count = module.is_executable ? 2 : 3;
        for (usize i = 0; i < list_count; ++i) {
            const auto tail_result = memory.read_u32(list_heads[i] + 0x04);
            if (tail_result.is_err()) return false;
            const u64 tail = tail_result.value();
            (void)memory.write_u32(list_links[i] + 0x00,
                                   static_cast<u32>(list_heads[i]));
            (void)memory.write_u32(list_links[i] + 0x04,
                                   static_cast<u32>(tail));
            (void)memory.write_u32(tail + 0x00,
                                   static_cast<u32>(list_links[i]));
            (void)memory.write_u32(list_heads[i] + 0x04,
                                   static_cast<u32>(list_links[i]));
        }
        (void)memory.write_u32(entry + 0x18,
                               static_cast<u32>(module.base_address));
        (void)memory.write_u32(entry + 0x1C,
                               static_cast<u32>(module.entry_point));
        (void)memory.write_u32(entry + 0x20,
                               static_cast<u32>(module.size_of_image));
        write_unicode_string32(memory, entry + 0x24, name, name_bytes);
        write_unicode_string32(memory, entry + 0x2C, base_name,
                               base_name_bytes);
        write_ldr_entry_tail(memory, false, ldr, entry, module,
                             std::string_view(path).substr(base_offset));
    }
    return true;
}

bool remove_peb_loader_module(GuestMemory& memory, u64 peb_address,
                              bool is_64bit, u64 base_address) {
    if (peb_address == 0 || base_address == 0) return false;

    const auto ldr_result = is_64bit
        ? memory.read_u64(peb_address + 0x18)
        : memory.read_u32(peb_address + 0x0C).map([](u32 value) {
              return static_cast<u64>(value);
          });
    if (ldr_result.is_err() || ldr_result.value() == 0) return false;

    const u64 ldr = ldr_result.value();
    const u64 head = ldr + (is_64bit ? 0x20 : 0x14);
    const u64 link_offset = is_64bit ? 0x10 : 0x08;
    std::unordered_set<u64> visited;
    u64 links = is_64bit ? memory.read_u64(head).value_or(0)
                         : memory.read_u32(head).map([](u32 value) {
                               return static_cast<u64>(value);
                           }).value_or(0);
    while (links != head) {
        if (links == 0 || !visited.insert(links).second || links < link_offset) {
            return false;
        }
        const u64 entry = links - link_offset;
        const auto candidate = is_64bit
            ? memory.read_u64(entry + 0x30)
            : memory.read_u32(entry + 0x18).map([](u32 value) {
                  return static_cast<u64>(value);
              });
        if (candidate.is_err()) return false;
        if (candidate.value() != base_address) {
            const auto next = is_64bit
                ? memory.read_u64(links)
                : memory.read_u32(links).map([](u32 value) {
                      return static_cast<u64>(value);
                  });
            if (next.is_err()) return false;
            links = next.value();
            continue;
        }

        if (is_64bit) {
            for (const u64 entry_links :
                 {entry + 0x00, entry + 0x10, entry + 0x20,
                  entry + kLdrEntry64.hash_links}) {
                const auto next = memory.read_u64(entry_links + 0x00);
                const auto previous = memory.read_u64(entry_links + 0x08);
                if (next.is_err() || previous.is_err()) return false;
                if (next.value() == 0 || previous.value() == 0) continue;
                (void)memory.write_u64(previous.value() + 0x00, next.value());
                (void)memory.write_u64(next.value() + 0x08, previous.value());
            }
        } else {
            for (const u64 entry_links :
                 {entry + 0x00, entry + 0x08, entry + 0x10,
                  entry + kLdrEntry32.hash_links}) {
                const auto next = memory.read_u32(entry_links + 0x00);
                const auto previous = memory.read_u32(entry_links + 0x04);
                if (next.is_err() || previous.is_err()) return false;
                if (next.value() == 0 || previous.value() == 0) continue;
                (void)memory.write_u32(previous.value() + 0x00, next.value());
                (void)memory.write_u32(next.value() + 0x04, previous.value());
            }
        }
        return memory.unmap_region(entry, ENTRY_PAGE_SIZE).is_ok();
    }
    return false;
}


VResult<TebInfo> setup_teb(GuestMemory& memory, cpu::CpuState& cpu,
                            const TebConfig& config) {
    constexpr u64 TEB_SIZE = 0x2000; // 8 KiB
    // The second 4 KiB is past every documented Win32 TEB field. Native 32-bit
    // execution stores per-thread trampoline pointers there
    // (cpu::x64::kTebNative32Collect32 / kTebNative32Sentinel).
    constexpr u64 PEB_SIZE = 0x1000; // 4 KiB

    const u64 TEB_ADDRESS = config.stack_base;
    const u64 PEB_ADDRESS = TEB_ADDRESS + TEB_SIZE;

    // Allocate TEB
    auto teb_result = memory.map_region(TEB_ADDRESS, TEB_SIZE,
                                         MemoryProtection::ReadWrite);
    if (teb_result.is_err()) {
        return VResult<TebInfo>::err(Error::OutOfMemory);
    }

    if (config.is_64bit) {
        // x64 TEB layout (NT_TIB64 + Win64 TEB)
        (void)memory.write_u64(TEB_ADDRESS + 0x00, 0);                    // ExceptionList
        (void)memory.write_u64(TEB_ADDRESS + 0x08, config.stack_base);    // StackBase
        (void)memory.write_u64(TEB_ADDRESS + 0x10, config.stack_bottom);  // StackLimit
        // DeallocationStack: reported by GetCurrentThreadStackLimits as
        // LowLimit, and the bottom of the range exception dispatch treats
        // as the stack's guard when recognising an overflow.
        if (config.deallocation_stack != 0) {
            (void)memory.write_u64(TEB_ADDRESS + 0x1478,
                                   config.deallocation_stack);
        }
        (void)memory.write_u64(TEB_ADDRESS + 0x18, 0);                    // SubSystemTib
        (void)memory.write_u64(TEB_ADDRESS + 0x20, 0);                    // FiberData
        (void)memory.write_u64(TEB_ADDRESS + 0x28, 0);                    // ArbitraryUserPointer
        (void)memory.write_u64(TEB_ADDRESS + 0x30, TEB_ADDRESS);          // Self
        (void)memory.write_u64(TEB_ADDRESS + 0x38, 0);                    // EnvironmentPointer
        (void)memory.write_u64(TEB_ADDRESS + 0x40, config.process_id);    // ClientId.UniqueProcess
        (void)memory.write_u64(TEB_ADDRESS + 0x48, config.thread_id);     // ClientId.UniqueThread
        (void)memory.write_u64(TEB_ADDRESS + 0x58, 0);                    // ThreadLocalStoragePointer
        (void)memory.write_u64(TEB_ADDRESS + 0x60, PEB_ADDRESS);          // PEB
        (void)memory.write_u32(TEB_ADDRESS + 0x68, 0);                    // LastErrorValue

        cpu.seg.gs_desc.base = TEB_ADDRESS;
        // Windows x64 user-mode segment selectors (WOW64 / native).
        // Exception dispatch reads them with 8C /r.
        cpu.seg.cs = 0x33;
        cpu.seg.ds = 0x2B;
        cpu.seg.es = 0x2B;
        cpu.seg.ss = 0x2B;
        cpu.seg.fs = 0x53;
        cpu.seg.gs = 0x2B;
    } else {
        // x86 TEB layout (NT_TIB32 + Win32 TEB)
        (void)memory.write_u32(TEB_ADDRESS + 0x00, 0xFFFFFFFF);                                 // ExceptionList (no handlers)
        (void)memory.write_u32(TEB_ADDRESS + 0x04, static_cast<u32>(config.stack_base));         // StackBase
        (void)memory.write_u32(TEB_ADDRESS + 0x08, static_cast<u32>(config.stack_bottom));       // StackLimit
        (void)memory.write_u32(TEB_ADDRESS + 0x0C, 0);                                          // SubSystemTib
        (void)memory.write_u32(TEB_ADDRESS + 0x10, 0);                                          // FiberData
        (void)memory.write_u32(TEB_ADDRESS + 0x14, 0);                                          // ArbitraryUserPointer
        (void)memory.write_u32(TEB_ADDRESS + 0x18, static_cast<u32>(TEB_ADDRESS));               // Self
        (void)memory.write_u32(TEB_ADDRESS + 0x1C, 0);                                          // EnvironmentPointer
        (void)memory.write_u32(TEB_ADDRESS + 0x20, config.process_id);                           // ClientId.UniqueProcess
        (void)memory.write_u32(TEB_ADDRESS + 0x24, config.thread_id);                            // ClientId.UniqueThread
        (void)memory.write_u32(TEB_ADDRESS + 0x2C, 0);                                          // ThreadLocalStoragePointer
        (void)memory.write_u32(TEB_ADDRESS + 0x30, static_cast<u32>(PEB_ADDRESS));               // PEB
        (void)memory.write_u32(TEB_ADDRESS + 0x34, 0);                                          // LastErrorValue

        cpu.seg.fs_desc.base = TEB_ADDRESS;
        // Windows x86 user-mode segment selectors. RtlCaptureContext and SEH
        // context-record building read them with 8C /r.
        cpu.seg.cs = 0x23;
        cpu.seg.ds = 0x2B;
        cpu.seg.es = 0x2B;
        cpu.seg.ss = 0x2B;
        cpu.seg.fs = 0x53;
        cpu.seg.gs = 0x2B;
    }

    // Allocate PEB
    auto peb_result = memory.map_region(PEB_ADDRESS, PEB_SIZE,
                                         MemoryProtection::ReadWrite);
    if (peb_result.is_ok()) {
        if (!setup_peb_loader_data(memory, PEB_ADDRESS, config.is_64bit,
                                   config.image_base, config.image_path,
                                   config.loaded_modules)) {
            return VResult<TebInfo>::err(Error::OutOfMemory);
        }
        // OSMajorVersion, OSMinorVersion, OSBuildNumber (WORD),
        // OSCSDVersion (WORD), OSPlatformId: the true version whatever the
        // image's manifest says (os_version_sources_probe "PEB").
        {
            const u64 os = PEB_ADDRESS + (config.is_64bit ? 0x118u : 0xA4u);
            (void)memory.write_u32(os + 0, kOsMajorVersion);
            (void)memory.write_u32(os + 4, kOsMinorVersion);
            (void)memory.write_u16(os + 8, static_cast<u16>(kOsBuildNumber));
            (void)memory.write_u16(os + 10, 0);
            (void)memory.write_u32(os + 12, kOsPlatformId);
        }
        if (config.is_64bit) {
            (void)memory.write_u64(PEB_ADDRESS + 0x10, config.image_base); // ImageBaseAddress

            // PEB->FastPebLock (+0x38) is a process-wide
            // PRTL_CRITICAL_SECTION used by the native runtime to serialize
            // access to mutable PEB state.  Keep a stable guest-visible lock;
            // leaving the pointer null is not a valid initialized process
            // environment and breaks code that inspects or acquires it.
            const u64 FAST_PEB_LOCK_ADDR = PEB_ADDRESS + 0x840;
            (void)memory.write_u64(PEB_ADDRESS + 0x38, FAST_PEB_LOCK_ADDR);
            (void)memory.write_u32(FAST_PEB_LOCK_ADDR + 0x08, 0xFFFFFFFFu);

            // Set up RTL_USER_PROCESS_PARAMETERS for 64-bit
            const u64 PARAMS_ADDR = PEB_ADDRESS + 0x400;
            constexpr u32 PARAMS_SIZE = 0x300;

            // Point PEB->ProcessParameters to our structure
            (void)memory.write_u64(PEB_ADDRESS + 0x20, PARAMS_ADDR);

            (void)memory.write_u32(PARAMS_ADDR + 0x00, PARAMS_SIZE);  // MaximumLength
            (void)memory.write_u32(PARAMS_ADDR + 0x04, PARAMS_SIZE);  // Length
            (void)memory.write_u32(PARAMS_ADDR + 0x08, 0x00000001);   // Flags (NORMALIZED)
            (void)memory.write_u32(PARAMS_ADDR + 0x0C, 0);            // DebugFlags

            // PEB->LoaderLock (x64 +0x110) is a PRTL_CRITICAL_SECTION. Guest
            // code reads PEB->LoaderLock->OwningThread (+0x10) for
            // loader-reentrancy checks. A null PEB->LoaderLock makes that
            // dereference an access violation, and a worker thread relaunches
            // in a loop. Point it at an unlocked CRITICAL_SECTION inside the
            // PEB page.
            const u64 LOADER_LOCK_ADDR = PEB_ADDRESS + 0x800;  // free, zeroed
            (void)memory.write_u64(PEB_ADDRESS + 0x110, LOADER_LOCK_ADDR);
            (void)memory.write_u32(LOADER_LOCK_ADDR + 0x08, 0xFFFFFFFFu);

            // x64 RTL_USER_PROCESS_PARAMETERS (winternl.h / Geoff Chappell):
            // ImagePathName @ +0x60, CommandLine @ +0x70, CurrentDirectory @ +0x38.
            // GetCommandLineW returns CommandLine.Buffer on Windows.
            const u64 img_buf = PEB_ADDRESS + 0x900;
            const std::string image = config.image_path.empty()
                                          ? std::string("C:\\vitreo.exe")
                                          : config.image_path;
            const u16 img_bytes = write_ascii_as_utf16(memory, img_buf, image);
            write_unicode_string64(memory, PARAMS_ADDR + 0x60, img_buf, img_bytes);

            u64 cmd_buf = config.command_line_va;
            u16 cmd_bytes = config.command_line_bytes;
            if (cmd_buf == 0) {
                cmd_buf = PEB_ADDRESS + 0xA80;
                const std::string cmd = config.command_line.empty()
                                            ? image
                                            : config.command_line;
                cmd_bytes = write_ascii_as_utf16(memory, cmd_buf, cmd);
            }
            write_unicode_string64(memory, PARAMS_ADDR + 0x70, cmd_buf, cmd_bytes);

            std::string cwd = image;
            const auto slash = cwd.find_last_of("\\/");
            if (slash != std::string::npos) {
                cwd.resize(slash + 1);
            } else {
                cwd = "C:\\";
            }
            const u64 cwd_buf = PEB_ADDRESS + 0xC00;
            const u16 cwd_bytes = write_ascii_as_utf16(memory, cwd_buf, cwd);
            write_unicode_string64(memory, PARAMS_ADDR + 0x38, cwd_buf, cwd_bytes);
        } else {
            (void)memory.write_u32(PEB_ADDRESS + 0x08, static_cast<u32>(config.image_base));

            // 32-bit PEB->FastPebLock is at +0x1C and points to the
            // corresponding 24-byte RTL_CRITICAL_SECTION layout.
            const u64 FAST_PEB_LOCK_ADDR = PEB_ADDRESS + 0x840;
            (void)memory.write_u32(
                PEB_ADDRESS + 0x1C,
                static_cast<u32>(FAST_PEB_LOCK_ADDR));
            (void)memory.write_u32(
                FAST_PEB_LOCK_ADDR + 0x04, 0xFFFFFFFFu);

            // Set up RTL_USER_PROCESS_PARAMETERS inside the PEB page
            // Place at PEB + 0x400 (PEB itself is ~0x248 bytes for 32-bit)
            const u64 PARAMS_ADDR = PEB_ADDRESS + 0x400;
            constexpr u32 PARAMS_SIZE = 0x300;

            // Point PEB->ProcessParameters to our structure
            (void)memory.write_u32(PEB_ADDRESS + 0x10, static_cast<u32>(PARAMS_ADDR));

            // RTL_USER_PROCESS_PARAMETERS fields (32-bit offsets)
            (void)memory.write_u32(PARAMS_ADDR + 0x00, PARAMS_SIZE);  // MaximumLength
            (void)memory.write_u32(PARAMS_ADDR + 0x04, PARAMS_SIZE);  // Length
            (void)memory.write_u32(PARAMS_ADDR + 0x08, 0x00000001);   // Flags (NORMALIZED)
            (void)memory.write_u32(PARAMS_ADDR + 0x0C, 0);            // DebugFlags
            (void)memory.write_u32(PARAMS_ADDR + 0x10, 0);            // ConsoleHandle
            (void)memory.write_u32(PARAMS_ADDR + 0x14, 0);            // ConsoleFlags
            (void)memory.write_u32(PARAMS_ADDR + 0x18, 0);            // StdInputHandle
            (void)memory.write_u32(PARAMS_ADDR + 0x1C, 0);            // StdOutputHandle
            (void)memory.write_u32(PARAMS_ADDR + 0x20, 0);            // StdErrorHandle

            // CurrentDirectory — UNICODE_STRING at offset 0x24
            // Use a generic default instead of app-specific paths.
            const u64 CUR_DIR_BUF = PARAMS_ADDR + 0x200;
            const char16_t cur_dir[] = u"C:\\";
            u32 cur_dir_bytes = static_cast<u32>((sizeof(cur_dir) / sizeof(char16_t) - 1) * 2);
            (void)memory.write_u16(PARAMS_ADDR + 0x24, static_cast<u16>(cur_dir_bytes));       // Length
            (void)memory.write_u16(PARAMS_ADDR + 0x26, static_cast<u16>(cur_dir_bytes + 2));   // MaximumLength
            (void)memory.write_u32(PARAMS_ADDR + 0x28, static_cast<u32>(CUR_DIR_BUF));         // Buffer
            for (u32 i = 0; cur_dir[i]; ++i) {
                (void)memory.write_u16(CUR_DIR_BUF + i * 2, static_cast<u16>(cur_dir[i]));
            }
            (void)memory.write_u16(CUR_DIR_BUF + cur_dir_bytes, 0); // null terminator

            // ImagePathName — UNICODE_STRING at offset 0x38
            // Reuse the same buffer area (PARAMS + 0x240)
            const u64 IMG_PATH_BUF = PARAMS_ADDR + 0x240;
            const char16_t img_path[] = u"C:\\vitreo.exe";
            u32 img_path_bytes = static_cast<u32>((sizeof(img_path) / sizeof(char16_t) - 1) * 2);
            (void)memory.write_u16(PARAMS_ADDR + 0x38, static_cast<u16>(img_path_bytes));
            (void)memory.write_u16(PARAMS_ADDR + 0x3A, static_cast<u16>(img_path_bytes + 2));
            (void)memory.write_u32(PARAMS_ADDR + 0x3C, static_cast<u32>(IMG_PATH_BUF));
            for (u32 i = 0; img_path[i]; ++i) {
                (void)memory.write_u16(IMG_PATH_BUF + i * 2, static_cast<u16>(img_path[i]));
            }
            (void)memory.write_u16(IMG_PATH_BUF + img_path_bytes, 0);

            // CommandLine — UNICODE_STRING at offset 0x40 (same as image path)
            (void)memory.write_u16(PARAMS_ADDR + 0x40, static_cast<u16>(img_path_bytes));
            (void)memory.write_u16(PARAMS_ADDR + 0x42, static_cast<u16>(img_path_bytes + 2));
            (void)memory.write_u32(PARAMS_ADDR + 0x44, static_cast<u32>(IMG_PATH_BUF));

            // Modern 32-bit PEB layout exposes LoaderLock at +0xA0. Some
            // runtime libraries inspect RTL_CRITICAL_SECTION::OwningThread
            // directly to avoid work while executing under the loader lock.
            // Keep a stable, unlocked guest-visible structure rather than a
            // null pointer that faults on the first inspection.
            const u64 LOADER_LOCK_ADDR = PEB_ADDRESS + 0x800;
            (void)memory.write_u32(
                PEB_ADDRESS + 0xA0,
                static_cast<u32>(LOADER_LOCK_ADDR));
            (void)memory.write_u32(
                LOADER_LOCK_ADDR + 0x04, 0xFFFFFFFFu); // LockCount = -1.
        }
    }

    return VResult<TebInfo>::ok(TebInfo{TEB_ADDRESS, PEB_ADDRESS});
}

} // namespace vitreo::pe
