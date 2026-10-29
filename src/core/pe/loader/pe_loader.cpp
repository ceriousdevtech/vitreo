// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE Loader Integration (PE-005)

#include <cstring>
#include "pe_loader.hpp"
#include "core/pe/loader/load_config.hpp"
#include "core/pe/loader/cor_image.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/reloc/relocator.hpp"
#include "core/pe/mapper/image_placement.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "core/pe/imports/iat_patcher.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vitreo::pe {

// VITREO_LOG_IMPORT_RESOLUTION: read once, not per call (getenv scans the
// environment block on every call).
static inline bool import_resolution_logging_enabled() {
    static const bool enabled =
        std::getenv("VITREO_LOG_IMPORT_RESOLUTION") != nullptr;
    return enabled;
}

// ============================================================================
// Construction
// ============================================================================

PeLoader::PeLoader(GuestMemory& memory, winapi::ThunkRegistry& thunks)
    : m_memory(memory)
    , m_thunks(thunks) {}

// ============================================================================
// Load
// ============================================================================

Result<LoadedImage, Error> PeLoader::load(
    std::span<const u8> pe_data,
    GuestAddr preferred_base,
    ImportCallback extra_resolver,
    PreImportCallback pre_import_callback,
    bool resolve_imports)
{
    // Step 1: Parse the PE file
    auto parse_result = parse_pe(pe_data);
    if (parse_result.is_err()) {
        return Result<LoadedImage, Error>::err(Error::InvalidFormat);
    }
    auto& pe = parse_result.value();

    // Step 2: Determine load address. Under identity mapping the preferred
    // base can belong to the host process, in which case a relocatable image
    // is rebased rather than refused.
    const auto placement = select_image_base(m_memory, pe, preferred_base);
    if (!placement.ok()) {
        std::fprintf(stderr,
                     "[pe] cannot place image: preferred=0x%llx size=0x%x: %s\n",
                     static_cast<unsigned long long>(
                         preferred_base != 0 ? preferred_base : pe.image_base),
                     pe.size_of_image, placement.failure);
        return Result<LoadedImage, Error>::err(Error::AlreadyExists);
    }
    if (placement.relocated) {
        std::fprintf(stderr,
                     "[pe] image rebased off host-owned memory: 0x%llx -> 0x%llx "
                     "size=0x%x\n",
                     static_cast<unsigned long long>(
                         preferred_base != 0 ? preferred_base : pe.image_base),
                     static_cast<unsigned long long>(placement.base),
                     pe.size_of_image);
    }
    GuestAddr base_address = placement.base;

    // Step 3: Map sections into guest memory
    SectionMapper mapper(m_memory);
    auto map_result = mapper.map(pe, pe_data, base_address);
    if (map_result.is_err()) {
        // Distinguish guest-region overlap (preferred-base collision, the
        // common case for dynamic DLL loads) from real host VM exhaustion.
        // SectionMapper reports SectionOverlap when GuestMemory::map_region
        // returns AlreadyExists.
        Error err = (map_result.error() == MapError::SectionOverlap)
                        ? Error::AlreadyExists
                        : Error::OutOfMemory;
        return Result<LoadedImage, Error>::err(err);
    }
    auto& mapped = map_result.value();
    const auto discard_mapped_image = [&]() {
        // Contiguous SizeOfImage mapping — one unmap at the image base.
        (void)mapper.unmap(mapped);
    };

    // The Windows loader applies relocations and patches the IAT before final
    // section protections are enforced. Our SectionMapper sets protections
    // based on section characteristics, which can make the IAT read-only (e.g.
    // when it resides in .rdata). Temporarily make all mapped sections
    // writable while we do relocations/import patching, then restore.
    // Header pages are PAGE_READONLY after map(); lift that too so the
    // OptionalHeader.ImageBase write below can land.
    if (mapped.header_size != 0) {
        auto pr = m_memory.protect_region(
            mapped.base_address, mapped.header_size, MemoryProtection::ReadWrite);
        if (!pr) {
            discard_mapped_image();
            return Result<LoadedImage, Error>::err(Error::InternalError);
        }
    }
    for (const auto& s : mapped.sections) {
        if (s.size == 0) {
            continue;
        }
        auto pr = m_memory.protect_region(s.guest_address, s.size, MemoryProtection::ReadWrite);
        if (!pr) {
            VITREO_TRACE(
                         "[pe] protect_region(RW) failed: base=0x%llx section=\"%s\" addr=0x%llx size=0x%llx\n",
                         static_cast<unsigned long long>(mapped.base_address),
                         s.name,
                         static_cast<unsigned long long>(s.guest_address),
                         static_cast<unsigned long long>(s.size));
            discard_mapped_image();
            return Result<LoadedImage, Error>::err(Error::InternalError);
        }
    }

    // An IL-only image is data to the Windows 8+ loader: it is neither
    // relocated nor bound, and its in-memory ImageBase keeps the file's
    // value (cor_image.hpp; cor_loader_probe{32,64}.expected).
    const bool il_only_image = is_il_only(read_cor_image(pe_data));

    // Step 4: Apply relocations if loaded at a different base
    if (!il_only_image && Relocator::needs_relocation(pe, base_address)) {
        Relocator relocator(m_memory);
        auto reloc_result = relocator.apply(pe, pe_data, base_address);
        if (reloc_result.is_err()) {
            VITREO_TRACE(
                         "[pe] relocation failed: base=0x%llx preferred=0x%llx err=%s\n",
                         static_cast<unsigned long long>(base_address),
                         static_cast<unsigned long long>(pe.image_base),
                         reloc_error_message(reloc_result.error()));
            discard_mapped_image();
            return Result<LoadedImage, Error>::err(Error::InternalError);
        } else {
            const auto& stats = reloc_result.value();
            VITREO_TRACE(
                         "[pe] relocations applied: base=0x%llx preferred=0x%llx delta=0x%llx blocks=%u entries=%u\n",
                         static_cast<unsigned long long>(base_address),
                         static_cast<unsigned long long>(pe.image_base),
                         static_cast<unsigned long long>(stats.delta),
                         stats.blocks_processed,
                         stats.entries_applied);
        }
    }

    // The Windows loader initializes the /GS cookie from the image load
    // configuration before any image code (including TLS callbacks) executes.
    // Do this while mapped sections are already writable for loader fixups.
    if (!initialize_load_config_security_cookie(
            m_memory, pe, mapped.base_address)) {
        discard_mapped_image();
        return Result<LoadedImage, Error>::err(Error::InvalidFormat);
    }

    if (pre_import_callback) {
        pre_import_callback(
            mapped.base_address, static_cast<usize>(pe.size_of_image),
            pe.is_64bit);
    }

    // Step 5: Resolve imports via thunk registry.
    // DONT_RESOLVE_DLL_REFERENCES / LoadLibrary of a non-DLL EXE skips this
    // (Learn: static imports are not loaded; DllMain is not called either —
    // that part is owned by ModuleManager).
    if (resolve_imports && !il_only_image && pe.has_imports()) {
        // Track imports that fall through to the synthetic missing-export
        // path so the loader can emit a single summary diagnostic. Per-call
        // logs are emitted by the stub itself when a missing import is
        // actually invoked.
        std::vector<std::string> stubbed;
        ImportCallback callback = [this, &extra_resolver, &stubbed](
            std::string_view dll_name,
            std::string_view func_name,
            u16 ordinal) -> u64
        {
            const auto trace_resolution = [&](u64 address,
                                              const char* source) {
                if (!import_resolution_logging_enabled()) {
                    return;
                }
                fprintf(stderr,
                        "[pe] import-resolution %.*s!%.*s ord=%u -> "
                        "0x%llx source=%s\n",
                        static_cast<int>(dll_name.size()), dll_name.data(),
                        static_cast<int>(func_name.size()), func_name.data(),
                        static_cast<unsigned>(ordinal),
                        static_cast<unsigned long long>(address),
                        source);
            };

            // Caller-supplied resolver gets first crack (e.g. companion-DLL
            // loading from the executing module's directory). It returns 0
            // to defer to the default thunk-registry path.
            if (extra_resolver) {
                u64 r = extra_resolver(dll_name, func_name, ordinal);
                if (r != 0) {
                    trace_resolution(r, "companion");
                    return r;
                }
            }
            const auto mapped = resolve_api_set(dll_name);

            // Try by name first
            if (!func_name.empty()) {
                auto result = m_thunks.get_thunk_address(dll_name, func_name);
                if (result.is_ok()) {
                    trace_resolution(result.value(), "registry-name");
                    return m_thunks.guest_entry(result.value());
                }
                if (mapped != dll_name) {
                    result = m_thunks.get_thunk_address(mapped, func_name);
                    if (result.is_ok()) {
                        trace_resolution(result.value(), "registry-api-set-name");
                        return m_thunks.guest_entry(result.value());
                    }
                }
            }
            // Try by ordinal
            if (ordinal != 0) {
                auto result = m_thunks.get_thunk_address(dll_name, ordinal);
                if (result.is_ok()) {
                    trace_resolution(result.value(), "registry-ordinal");
                    return m_thunks.guest_entry(result.value());
                }
                if (mapped != dll_name) {
                    result = m_thunks.get_thunk_address(mapped, ordinal);
                    if (result.is_ok()) {
                        trace_resolution(result.value(), "registry-api-set-ordinal");
                        return m_thunks.guest_entry(result.value());
                    }
                }
            }
            // Data exports (msvcrt's _iob, _pctype, ...) patch the IAT with the
            // address of the variable itself; the guest dereferences the slot
            // as data, so a callable thunk would be wrong here.
            if (!func_name.empty()) {
                u64 data_addr = m_thunks.lookup_data_export(dll_name, func_name);
                if (data_addr != 0) {
                    trace_resolution(data_addr, "registry-data");
                    return data_addr;
                }
                if (mapped != dll_name) {
                    data_addr = m_thunks.lookup_data_export(mapped, func_name);
                    if (data_addr != 0) {
                        trace_resolution(data_addr, "registry-api-set-data");
                        return data_addr;
                    }
                }
            }
            // Missing-export stubs are only for unimplemented functions of
            // a host WinAPI module already in the thunk registry. Microsoft
            // Learn ("Load-Time Dynamic Linking"): if the system cannot
            // locate a required DLL, it terminates the process (and
            // LoadLibrary fails). Vendor/app DLLs that are neither
            // registered nor found on disk must not receive a synthetic
            // IAT stub.
            if (!m_thunks.has_dll(dll_name) && !m_thunks.has_dll(mapped)) {
                trace_resolution(0, "dll-not-found");
                return 0;
            }

            // Last resort: install a synthetic "missing export" stub so the
            // IAT slot points at a logging handler instead of NULL. Without
            // this, calls through unresolved imports crash via NULL-deref
            // SEH. The stub logs the missing export and returns the
            // documented failure instead of leaving a null IAT slot.
            std::string label;
            label.append(dll_name);
            label.push_back('!');
            if (!func_name.empty()) {
                label.append(func_name);
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "#%u",
                              static_cast<unsigned>(ordinal));
                label.append(buf);
            }
            stubbed.push_back(std::move(label));
            u64 address =
                m_thunks.register_missing_export(dll_name, func_name, ordinal);
            trace_resolution(address, "missing-export");
            return address;
        };

        auto import_result = resolve_and_patch_imports(
            m_memory, pe, pe_data, base_address, callback);
        if (import_result.is_err()) {
            fprintf(stderr,
                    "[pe] import patching failed @0x%llx: %s\n",
                    static_cast<unsigned long long>(base_address),
                    import_error_message(import_result.error()));
            discard_mapped_image();
            return Result<LoadedImage, Error>::err(Error::InternalError);
        }
        const auto& patch = import_result.value();
        if (import_resolution_logging_enabled()) {
            const auto& stats = patch.stats;
            fprintf(stderr,
                    "[pe] import-patch-summary base=0x%llx modules=%u "
                    "resolved=%u unresolved=%u\n",
                    static_cast<unsigned long long>(base_address),
                    stats.modules_processed,
                    stats.imports_resolved,
                    stats.imports_unresolved);
        }
        if (patch.stats.imports_unresolved != 0) {
            fprintf(stderr,
                    "[pe] required DLL not found @0x%llx: %u unresolved "
                    "imports (STATUS_DLL_NOT_FOUND)\n",
                    static_cast<unsigned long long>(base_address),
                    patch.stats.imports_unresolved);
            for (const auto& unresolved : patch.unresolved) {
                fprintf(stderr, "[pe]   unresolved: %s!%s (ord %u)\n",
                        unresolved.dll_name.c_str(),
                        unresolved.function_name.c_str(),
                        unresolved.ordinal);
            }
            discard_mapped_image();
            return Result<LoadedImage, Error>::err(Error::NotFound);
        }
        // Host-DLL missing functions are stubbed above, so unresolved is 0.
        // Surface the count of synthetic stubs so users see at load time
        // which imports will fail-with-log if called.
        if (!stubbed.empty()) {
            fprintf(stderr,
                    "[pe] WARNING: %zu unresolved imports @0x%llx "
                    "stubbed with logging fail-thunks "
                    "(calls will return 0xFFFFFFFF)\n",
                    stubbed.size(),
                    static_cast<unsigned long long>(base_address));
            for (const auto& name : stubbed) {
                fprintf(stderr, "[pe]   stubbed: %s\n", name.c_str());
            }
        }
    }

    // Windows updates the in-memory OptionalHeader.ImageBase to the actual
    // load address. Packers and runtime helpers that read the mapped PE header
    // for their delta expect this. Must happen while header pages are still
    // writable (before the PAGE_READONLY restore below).
    if (auto lfanew = m_memory.read_u32(mapped.base_address + 0x3Cu);
        !il_only_image && lfanew.is_ok() && lfanew.value() != 0) {
        const GuestAddr opt_base =
            mapped.base_address + lfanew.value() + 4u + sizeof(CoffHeader);
        if (pe.is_64bit) {
            (void)m_memory.write_u64(opt_base + 24u, mapped.base_address);
        } else {
            (void)m_memory.write_u32(
                opt_base + 28u, static_cast<u32>(mapped.base_address));
        }
    }

    // Restore final section protections (best-effort).
    for (const auto& s : mapped.sections) {
        if (s.size == 0) {
            continue;
        }
        (void)m_memory.protect_region(s.guest_address, s.size, s.protection);
    }
    if (mapped.header_size != 0) {
        (void)m_memory.protect_region(
            mapped.base_address, mapped.header_size, MemoryProtection::Read);
    }

    m_memory.images().register_image(GuestImageIdentity{
        mapped.base_address, mapped.base_address + mapped.image_size,
        GuestImageRegistry::identity_from_file(pe_data, pe.size_of_headers,
                                               pe.image_base),
        pe.image_base});

    // Step 6: Build and return LoadedImage
    LoadedImage loaded{};
    // An IL-only image has no loader entry (LDR EntryPoint NULL on Windows
    // 8+): nothing in it runs as DllMain.
    loaded.entry_point = il_only_image ? 0 : mapped.entry_point;
    loaded.image_base = mapped.base_address;
    loaded.image_end = mapped.base_address + mapped.image_size;
    loaded.bitness = pe.is_64bit ? 64u : 32u;
    loaded.header_size = mapped.image_size;
    loaded.sections = std::move(mapped.sections);

    return Result<LoadedImage, Error>::ok(std::move(loaded));
}

void PeLoader::unload(const LoadedImage& image) {
    const usize image_size = image.image_end > image.image_base
        ? static_cast<usize>(image.image_end - image.image_base)
        : image.header_size;
    if (image_size != 0) {
        (void)m_memory.unmap_region(image.image_base, image_size);
    }
}

} // namespace vitreo::pe
