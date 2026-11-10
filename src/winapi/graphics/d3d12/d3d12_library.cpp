// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_library.cpp
/// @brief ID3D12PipelineLibrary1, ID3D12ShaderCacheSession and the device's
/// shader-cache controls (ID3D12Device9::ShaderCacheControl,
/// ID3D12Device6::SetBackgroundProcessingMode).
///
/// Sources: Microsoft Learn (ID3D12PipelineLibrary, ID3D12PipelineLibrary1,
/// ID3D12Device1::CreatePipelineLibrary, ID3D12ShaderCacheSession,
/// D3D12_SHADER_CACHE_SESSION_DESC, ID3D12Device9::ShaderCacheControl,
/// ID3D12Device6::SetBackgroundProcessingMode), Windows SDK 10.0.26100.0
/// um/d3d12.h, and tools/oracle_probe/d3d12_a11_library_cache_probe on WARP
/// and NVIDIA (identical except where noted).
///
/// A library entry is the stored pipeline's description digest
/// (Frontend::pipeline_digest: shader bytecode, root signature blob, every
/// state field) under its name. Load* rebuilds the pipeline from the
/// description the caller passes -- Learn: it must match the stored one --
/// and hands out the stored object while it is alive, so a pipeline is
/// compiled from bytecode either way; the blob carries no translated code.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdio>
#include <cstring>

#include <sys/sysctl.h>

namespace vitreo::winapi::d3d12 {

namespace {

/// Serialized library: header, then entries.
///   0  char[8] magic "VTRPLIB1"
///   8  u32 format version
///  12  u32 entry count
///  16  u64 adapter key (LUID and backend device name)
///  24  u64 driver key (format and digest rules)
///  32  u64 checksum of the entry bytes
///  40  entries: u32 name units, u16 name[units], u64 digest,
///      u8 compute, u8 from_stream, u16 0
constexpr char kLibraryMagic[8] = {'V', 'T', 'R', 'P', 'L', 'I', 'B', '1'};
constexpr u32 kLibraryFormat = 1;
constexpr usize kLibraryHeader = 40;
/// Changes whenever pipeline_digest's rules change: a blob from another
/// version is D3D12_ERROR_DRIVER_VERSION_MISMATCH (Learn).
constexpr u64 kLibraryDriverKey = 0x56545250'00000001ull;

void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (i * 8)));
}
void put_u64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(v >> (i * 8)));
}
u32 get_u32(const u8* p) {
    u32 v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
u64 get_u64(const u8* p) {
    u64 v = 0;
    std::memcpy(&v, p, 8);
    return v;
}

/// Measured (a11 cache): a non-zero in-memory size must exceed 36 KiB
/// (36864 is E_INVALIDARG, 36865 S_OK); 0 is the default, 1 MiB, and an
/// entry count of 0 the default 128.
constexpr u32 kMinimumMemoryCacheBytes = 36864;
constexpr u32 kDefaultMemoryCacheBytes = 1u << 20;
constexpr u32 kDefaultMemoryCacheEntries = 128;

}  // namespace

// =============================================================================
// ID3D12PipelineLibrary1
// =============================================================================

u64 Frontend::library_adapter_key() const {
    u64 h = fnv1a(&dxgi::ADAPTER_LUID_LOW, sizeof(dxgi::ADAPTER_LUID_LOW));
    h = fnv1a(&dxgi::ADAPTER_LUID_HIGH, sizeof(dxgi::ADAPTER_LUID_HIGH), h);
    const std::string name = m_backend.backend_device_name();
    return fnv1a(name.data(), name.size(), h);
}

std::vector<u8> Frontend::serialize_library(const PipelineLibrary& library) const {
    std::vector<u8> entries;
    for (const auto& entry : library.entries) {
        put_u32(entries, static_cast<u32>(entry.name.size()));
        for (const char16_t unit : entry.name) {
            entries.push_back(static_cast<u8>(unit));
            entries.push_back(static_cast<u8>(unit >> 8));
        }
        put_u64(entries, entry.digest);
        entries.push_back(entry.compute ? 1 : 0);
        entries.push_back(entry.from_stream ? 1 : 0);
        entries.push_back(0);
        entries.push_back(0);
    }
    std::vector<u8> out(kLibraryMagic, kLibraryMagic + 8);
    put_u32(out, kLibraryFormat);
    put_u32(out, static_cast<u32>(library.entries.size()));
    put_u64(out, library_adapter_key());
    put_u64(out, kLibraryDriverKey);
    put_u64(out, fnv1a(entries.data(), entries.size()));
    out.insert(out.end(), entries.begin(), entries.end());
    return out;
}

u32 Frontend::deserialize_library(const std::vector<u8>& blob,
                                  PipelineLibrary& library) const {
    // Measured (a11 library): a truncated blob, a flipped byte anywhere and
    // garbage are E_INVALIDARG. Learn: a blob from another adapter is
    // D3D12_ERROR_ADAPTER_NOT_FOUND, from another driver version
    // D3D12_ERROR_DRIVER_VERSION_MISMATCH.
    if (blob.size() < kLibraryHeader ||
        std::memcmp(blob.data(), kLibraryMagic, 8) != 0) {
        return E_INVALIDARG;
    }
    if (get_u32(blob.data() + 8) != kLibraryFormat ||
        get_u64(blob.data() + 24) != kLibraryDriverKey) {
        return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
    }
    if (get_u64(blob.data() + 16) != library_adapter_key()) {
        return D3D12_ERROR_ADAPTER_NOT_FOUND;
    }
    const u32 count = get_u32(blob.data() + 12);
    const u8* p = blob.data() + kLibraryHeader;
    const u8* end = blob.data() + blob.size();
    if (fnv1a(p, static_cast<usize>(end - p)) != get_u64(blob.data() + 32)) {
        return E_INVALIDARG;
    }
    for (u32 i = 0; i < count; ++i) {
        if (end - p < 4) return E_INVALIDARG;
        const u32 units = get_u32(p);
        p += 4;
        if (static_cast<u64>(end - p) < static_cast<u64>(units) * 2 + 12) {
            return E_INVALIDARG;
        }
        PipelineLibrary::Entry entry;
        for (u32 u = 0; u < units; ++u) {
            entry.name.push_back(static_cast<char16_t>(p[0] | (p[1] << 8)));
            p += 2;
        }
        entry.digest = get_u64(p);
        entry.compute = p[8] != 0;
        entry.from_stream = p[9] != 0;
        p += 12;
        library.entries.push_back(std::move(entry));
    }
    if (p != end) return E_INVALIDARG;
    return S_OK;
}

bool Frontend::read_wide_name(GuestAddr address, std::u16string& out) const {
    out.clear();
    if (address == 0) return false;
    for (u32 index = 0; index < 4096; ++index) {
        const auto unit = m_memory.read_u16(address + index * 2);
        if (unit.is_err()) return false;
        if (unit.value() == 0) return true;
        out.push_back(static_cast<char16_t>(unit.value()));
    }
    return false;
}

void Frontend::create_pipeline_library(Marshaler& m, Device& device) {
    // CreatePipelineLibrary(const void *pLibraryBlob, SIZE_T BlobLength,
    //     REFIID riid, void **ppPipelineLibrary). Measured (a11 library):
    //     NULL/0 and (blob, 0) are an empty library; ppv NULL S_FALSE; an
    //     unanswered riid E_NOINTERFACE with *ppv NULL.
    const GuestAddr blob = m.arg_ptr(1);
    const u64 length = m.arg(2);
    const GuestAddr out = m.arg_ptr(4);
    if (out != 0) (void)write_pointer(out, 0);
    auto library = std::make_unique<PipelineLibrary>();
    library->device = device.address;
    if (blob != 0 && length != 0) {
        if (length > (256ull << 20)) {
            m.set_return(refuse(E_INVALIDARG,
                                "CreatePipelineLibrary: BlobLength %llu exceeds 256 MiB",
                                static_cast<unsigned long long>(length)));
            return;
        }
        std::vector<u8> bytes(static_cast<usize>(length));
        if (m_memory.read_bytes(blob, reinterpret_cast<Byte*>(bytes.data()),
                                bytes.size())
                .is_err()) {
            m.set_return(refuse(E_INVALIDARG, "CreatePipelineLibrary: the blob is unreadable"));
            return;
        }
        if (u32 error = deserialize_library(bytes, *library); error != S_OK) {
            m.set_return(refuse(error,
                                "CreatePipelineLibrary: the blob is not one Serialize made "
                                "on this device"));
            return;
        }
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    m.set_return(return_new_object(std::move(library), m.arg_ptr(3), out));
}

void Frontend::pipeline_library_load(Marshaler& m, PipelineLibrary& library,
                                     int kind) {
    // (pName, pDesc, riid, ppPipelineState); kind 0 graphics desc, 1
    // compute desc, 2 stream (ID3D12PipelineLibrary1::LoadPipeline).
    const GuestAddr out = m.arg_ptr(4);
    if (out != 0) (void)write_pointer(out, 0);
    std::u16string name;
    PipelineLibrary::Entry* entry = nullptr;
    if (read_wide_name(m.arg_ptr(1), name)) {
        for (auto& candidate : library.entries) {
            if (candidate.name == name) entry = &candidate;
        }
    }
    Device* device = find<Device>(library.device, Kind::Device);
    // Measured: a name never stored is E_INVALIDARG.
    if (entry == nullptr || device == nullptr) {
        m.set_return(refuse(E_INVALIDARG,
                            "ID3D12PipelineLibrary::Load*: %s",
                            device == nullptr ? "the library's device is gone"
                                              : "no pipeline is stored under that name"));
        return;
    }
    const GuestAddr library_address = library.address;
    const u64 library_serial = library.serial;
    std::unique_ptr<PipelineState> built;
    if (kind == 0) {
        create_graphics_pipeline_state(m, *device, 2, &built);
    } else if (kind == 1) {
        create_compute_pipeline_state(m, *device, 2, &built);
    } else {
        create_pipeline_state(m, *device, 2, &built);
    }
    if (!built) return;  // the creator's own failure is already returned
    // The build ran outside the frontend lock (build_pipeline_unlocked):
    // another thread may have stored into this library (moving its entries)
    // or released it meanwhile.
    PipelineLibrary* current = find_serial<PipelineLibrary>(
        library_address, library_serial, Kind::PipelineLibrary);
    entry = nullptr;
    if (current != nullptr) {
        for (auto& candidate : current->entries) {
            if (candidate.name == name) entry = &candidate;
        }
    }
    if (entry == nullptr) {
        m.set_return(refuse(E_INVALIDARG,
                            "ID3D12PipelineLibrary::Load*: the library or its entry went away "
                            "during the build"));
        return;
    }
    // Measured: another shader, root signature or state field, the other
    // kind (graphics vs compute), or the other creation path (desc vs
    // stream) is E_INVALIDARG with *ppv NULL. A reordered stream matches.
    if (built->compute != entry->compute ||
        built->from_stream != entry->from_stream ||
        built->cache_digest != entry->digest) {
        m.set_return(refuse(E_INVALIDARG,
                            "ID3D12PipelineLibrary::Load*: the description differs from the "
                            "stored pipeline's (%s)",
                            built->compute != entry->compute         ? "graphics vs compute"
                            : built->from_stream != entry->from_stream ? "desc vs stream"
                                                                       : "digest"));
        return;
    }
    // Measured: while the stored (or first loaded) pipeline lives, every
    // Load hands out that object.
    if (PipelineState* live = find_serial<PipelineState>(
            entry->live, entry->live_serial, Kind::PipelineState)) {
        GuidBytes iid{};
        const GuestAddr answer =
            read_guid(m.arg_ptr(3), iid) ? query_interface(*live, iid) : 0;
        if (answer == 0) {
            m.set_return(E_NOINTERFACE);
            return;
        }
        (void)write_pointer(out, answer);
        m.set_return(S_OK);
        return;
    }
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    const u32 hr = return_new_object(std::move(built), m.arg_ptr(3), out);
    if (hr == S_OK) {
        GuestAddr address = 0;
        (void)read_pointer(out, address);
        if (auto* pso = find<PipelineState>(address, Kind::PipelineState)) {
            entry->live = pso->address;
            entry->live_serial = pso->serial;
        }
    }
    m.set_return(hr);
}

void Frontend::pipeline_library_method(Marshaler& m, PipelineLibrary& library,
                                       usize slot) {
    switch (slot) {
    case PipelineLibrarySlot::StorePipeline: {
        // StorePipeline(LPCWSTR pName, ID3D12PipelineState *pPipeline).
        // Measured: a NULL name, or one already stored (a loaded blob's
        // names included), is E_INVALIDARG; the PSO's public reference count
        // does not change. (A NULL pipeline faults in d3d12.dll; refused.)
        std::u16string name;
        PipelineState* pso =
            find<PipelineState>(m.arg_ptr(2), Kind::PipelineState);
        if (!read_wide_name(m.arg_ptr(1), name) || pso == nullptr) {
            m.set_return(E_INVALIDARG);
            return;
        }
        for (const auto& entry : library.entries) {
            if (entry.name == name) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
        PipelineLibrary::Entry entry;
        entry.name = std::move(name);
        entry.digest = pso->cache_digest;
        entry.compute = pso->compute;
        entry.from_stream = pso->from_stream;
        entry.live = pso->address;
        entry.live_serial = pso->serial;
        library.entries.push_back(std::move(entry));
        m.set_return(S_OK);
        return;
    }
    case PipelineLibrarySlot::LoadGraphicsPipeline:
        pipeline_library_load(m, library, 0);
        return;
    case PipelineLibrarySlot::LoadComputePipeline:
        pipeline_library_load(m, library, 1);
        return;
    case PipelineLibrarySlot::LoadPipeline:
        pipeline_library_load(m, library, 2);
        return;
    case PipelineLibrarySlot::GetSerializedSize:
        // SIZE_T; never 0 (measured: an empty library's is not).
        m.set_return(static_cast<u64>(serialize_library(library).size()));
        return;
    case PipelineLibrarySlot::Serialize: {
        // Serialize(void *pData, SIZE_T DataSizeInBytes). Measured: a size
        // below GetSerializedSize is E_INVALIDARG; a larger one is S_OK.
        // (pData NULL faults in d3d12.dll; refused.)
        const GuestAddr data = m.arg_ptr(1);
        const u64 size = m.arg(2);
        const std::vector<u8> bytes = serialize_library(library);
        if (data == 0 || size < bytes.size()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (m_memory.write_bytes(data, reinterpret_cast<const Byte*>(bytes.data()),
                                 bytes.size())
                .is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        m.set_return(S_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

// =============================================================================
// ID3D12ShaderCacheSession
// =============================================================================

void Frontend::create_shader_cache_session(Marshaler& m, Device& device) {
    // CreateShaderCacheSession(const D3D12_SHADER_CACHE_SESSION_DESC *pDesc,
    //     REFIID riid, void **ppvSession). Measured (a11 cache).
    const GuestAddr desc = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(3);
    if (out != 0) (void)write_pointer(out, 0);
    if (desc == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto& l = layout::D3D12_SHADER_CACHE_SESSION_DESC[m_is_64bit];
    auto session = std::make_unique<ShaderCacheSession>();
    session->device = device.address;
    if (m_memory.read_bytes(desc + l.Identifier,
                            reinterpret_cast<Byte*>(session->identifier.data()),
                            16)
            .is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    session->mode = read_u32(desc + l.Mode);
    session->flags = read_u32(desc + l.Flags);
    session->memory_bytes = read_u32(desc + l.MaximumInMemoryCacheSizeBytes);
    session->memory_entries = read_u32(desc + l.MaximumInMemoryCacheEntries);
    session->file_bytes = read_u32(desc + l.MaximumValueFileSizeBytes);
    session->version = read_u64(desc + l.Version);
    constexpr u32 kFlags = D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED |
                           D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR;
    const bool disk = session->mode == D3D12_SHADER_CACHE_MODE_DISK;
    // Measured (a11 cache, a11d disk1/disk2, WARP and NVIDIA): a memory
    // cache needs MaximumInMemoryCacheSizeBytes 0 or above 36864; a disk
    // cache takes any memory size (16 is S_OK) but needs
    // MaximumValueFileSizeBytes 0 or above 36864 (36864 E_INVALIDARG,
    // 36865 S_OK).
    if (session->mode > D3D12_SHADER_CACHE_MODE_DISK ||
        (session->flags & ~kFlags) != 0 ||
        (!disk && session->memory_bytes != 0 &&
         session->memory_bytes <= kMinimumMemoryCacheBytes) ||
        (disk && session->file_bytes != 0 &&
         session->file_bytes <= kMinimumMemoryCacheBytes)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (session->memory_bytes == 0) {
        session->memory_bytes = kDefaultMemoryCacheBytes;
    }
    if (session->memory_entries == 0) {
        session->memory_entries = kDefaultMemoryCacheEntries;
    }
    // Learn (D3D12_SHADER_CACHE_SESSION_DESC::Identifier): sessions of one
    // identifier share the cache in the process; another Version clears it
    // when no session is open and is DXGI_ERROR_ALREADY_EXISTS otherwise
    // (measured for memory caches too, a11 cache; and for disk caches,
    // a11d disk2). Measured (a11d disk1): a disk session of an identifier
    // whose cache is open finds that cache whatever its location flag.
    auto found = m_shader_caches.find(session->identifier);
    if (found == m_shader_caches.end() && out == 0) {
        // Validation only: measured (a11d disk2), no file is created.
        m.set_return(S_FALSE);
        return;
    }
    if (found == m_shader_caches.end()) {
        auto created = std::make_shared<ShaderCacheStore>();
        created->version = session->version;
        if (disk) {
            const u32 result = open_disk_shader_cache(*session, *created);
            if (result != S_OK) {
                m.set_return(result);
                return;
            }
        }
        found = m_shader_caches.emplace(session->identifier, std::move(created)).first;
    }
    auto& store = found->second;
    if (store->version != session->version) {
        if (store->open_sessions != 0) {
            m.set_return(DXGI_ERROR_ALREADY_EXISTS);
            return;
        }
        store->values.clear();
        store->version = session->version;
        if (store->file) store->file->reset(session->version);
    }
    if (out == 0) {
        m.set_return(S_FALSE);
        return;
    }
    session->store = store;
    ++store->open_sessions;
    m.set_return(return_new_object(std::move(session), m.arg_ptr(2), out));
}

u32 Frontend::open_disk_shader_cache(const ShaderCacheSession& session,
                                     ShaderCacheStore& store) {
    // Learn (D3D12_SHADER_CACHE_FLAGS): caches live in temporary storage,
    // or in the current working directory with USE_WORKING_DIR; a
    // DRIVER_VERSIONED cache is versioned by the driver, side by side per
    // adapter (measured, a11d disk3: Windows adds the adapter's PCI ids to
    // the file name). Here the driver is the Metal device on this macOS
    // build, named by a hash of both.
    const bool working = (session.flags & D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR) != 0;
    const auto& source = working ? m_shader_cache_dirs.working_dir
                                 : m_shader_cache_dirs.temporary;
    const std::string dir = source ? source() : std::string{};
    if (dir.empty()) {
        unimplemented("ID3D12Device9::CreateShaderCacheSession",
                      "D3D12_SHADER_CACHE_MODE_DISK with no capsule directory");
        return E_FAIL;
    }
    std::string tag;
    if ((session.flags & D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED) != 0) {
        tag = shader_cache_driver_tag();
    }
    // MaximumValueFileSizeBytes 0 is 128 MiB (Learn), still reported as 0
    // by GetDesc (measured).
    constexpr u64 kDefaultFileBytes = 128ull << 20;
    const u64 limit = session.file_bytes != 0 ? session.file_bytes : kDefaultFileBytes;
    store.file = ShaderCacheFile::open(
        dir + "/" + shader_cache_file_name(session.identifier, tag), limit);
    if (!store.file) {
        unimplemented("ID3D12Device9::CreateShaderCacheSession",
                      "the disk cache file cannot be created");
        return E_FAIL;
    }
    store.file->load(session.version, store.values);
    return S_OK;
}

std::string Frontend::shader_cache_driver_tag() const {
    std::string identity = m_backend.backend_device_name();
    char build[64] = {};
    size_t length = sizeof(build);
    if (::sysctlbyname("kern.osversion", build, &length, nullptr, 0) == 0) {
        identity += "|";
        identity += build;
    }
    u64 hash = 1469598103934665603ull;
    for (const char c : identity) {
        hash = (hash ^ static_cast<u8>(c)) * 1099511628211ull;
    }
    char tag[32];
    std::snprintf(tag, sizeof(tag), "DRV_%016llX", static_cast<unsigned long long>(hash));
    return tag;
}

void Frontend::release_shader_cache_session(ShaderCacheSession& session) {
    if (!session.store) return;
    if (--session.store->open_sessions == 0 && session.store->delete_on_destroy) {
        // Measured: after SetDeleteOnDestroy and the last session's
        // release, a new session of that identifier finds nothing -- for a
        // disk cache in a new process too (a11d disk2): the file goes.
        if (session.store->file) session.store->file->remove();
        m_shader_caches.erase(session.identifier);
    }
    session.store.reset();
}

void Frontend::shader_cache_session_method(Marshaler& m,
                                           ShaderCacheSession& session,
                                           usize slot) {
    ShaderCacheStore& store = *session.store;
    switch (slot) {
    case ShaderCacheSessionSlot::FindValue: {
        // FindValue(const void *pKey, UINT KeySize, void *pValue,
        //     UINT *pValueSize). Measured: pValueSize NULL E_INVALIDARG; an
        //     absent key (a prefix included) DXGI_ERROR_NOT_FOUND, size
        //     untouched; pValue NULL reports the size; a smaller buffer
        //     DXGI_ERROR_MORE_DATA with the size, nothing copied.
        const GuestAddr key = m.arg_ptr(1);
        const u32 key_size = m.arg32(2);
        const GuestAddr value = m.arg_ptr(3);
        const GuestAddr size_address = m.arg_ptr(4);
        std::vector<u8> key_bytes(key_size);
        if (size_address == 0 || key == 0 || key_size == 0 ||
            m_memory.read_bytes(key, reinterpret_cast<Byte*>(key_bytes.data()),
                                key_bytes.size())
                .is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        auto found = store.values.find(key_bytes);
        if (found == store.values.end() && store.file) {
            // Another process may have stored it since.
            store.file->catch_up(store.values);
            found = store.values.find(key_bytes);
        }
        if (found == store.values.end()) {
            m.set_return(DXGI_ERROR_NOT_FOUND);
            return;
        }
        const u32 have = static_cast<u32>(found->second.size());
        const u32 room = read_u32(size_address);
        (void)m_memory.write_u32(size_address, have);
        if (value == 0) {
            m.set_return(S_OK);
            return;
        }
        if (room < have) {
            m.set_return(DXGI_ERROR_MORE_DATA);
            return;
        }
        (void)m_memory.write_bytes(
            value, reinterpret_cast<const Byte*>(found->second.data()), have);
        m.set_return(S_OK);
        return;
    }
    case ShaderCacheSessionSlot::StoreValue: {
        // StoreValue(pKey, KeySize, pValue, ValueSize). Measured: a NULL or
        // empty key or value E_INVALIDARG; a stored key
        // DXGI_ERROR_ALREADY_EXISTS. The entry and size limits evict
        // nothing measurable (two values in a one-entry cache are both
        // found), so none is applied.
        const GuestAddr key = m.arg_ptr(1);
        const u32 key_size = m.arg32(2);
        const GuestAddr value = m.arg_ptr(3);
        const u32 value_size = m.arg32(4);
        if (key == 0 || key_size == 0 || value == 0 || value_size == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        std::vector<u8> key_bytes(key_size);
        std::vector<u8> value_bytes(value_size);
        if (m_memory.read_bytes(key, reinterpret_cast<Byte*>(key_bytes.data()),
                                key_size)
                .is_err() ||
            m_memory.read_bytes(value,
                                reinterpret_cast<Byte*>(value_bytes.data()),
                                value_size)
                .is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (store.values.count(key_bytes) != 0) {
            m.set_return(DXGI_ERROR_ALREADY_EXISTS);
            return;
        }
        if (store.file) {
            // Measured (a11d disk2): a value that would take the file past
            // MaximumValueFileSizeBytes is DXGI_ERROR_CACHE_FULL.
            m.set_return(store.file->append(key_bytes, value_bytes, store.values));
            return;
        }
        store.values.emplace(std::move(key_bytes), std::move(value_bytes));
        m.set_return(S_OK);
        return;
    }
    case ShaderCacheSessionSlot::SetDeleteOnDestroy:
        store.delete_on_destroy = true;
        m.set_return(0);
        return;
    case ShaderCacheSessionSlot::GetDesc: {
        // Hidden return pointer; the defaults filled in (measured: size
        // 1048576, entries 128, file size as given).
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            const auto& l = layout::D3D12_SHADER_CACHE_SESSION_DESC[m_is_64bit];
            (void)m_memory.write_bytes(
                out + l.Identifier,
                reinterpret_cast<const Byte*>(session.identifier.data()), 16);
            (void)m_memory.write_u32(out + l.Mode, session.mode);
            (void)m_memory.write_u32(out + l.Flags, session.flags);
            (void)m_memory.write_u32(out + l.MaximumInMemoryCacheSizeBytes,
                                     session.memory_bytes);
            (void)m_memory.write_u32(out + l.MaximumInMemoryCacheEntries,
                                     session.memory_entries);
            (void)m_memory.write_u32(out + l.MaximumValueFileSizeBytes,
                                     session.file_bytes);
            (void)m_memory.write_u64(out + l.Version, session.version);
        }
        m.set_return(out);
        return;
    }
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

// =============================================================================
// ID3D12Device9::ShaderCacheControl, ID3D12Device6::SetBackgroundProcessingMode
// =============================================================================

u32 Frontend::shader_cache_control(u32 kinds, u32 control) {
    // Measured (a11 control, WARP and NVIDIA, no developer mode): no kind,
    // or a control that is not exactly one of DISABLE / ENABLE / CLEAR, is
    // E_INVALIDARG; ENABLE of a cache that is not disabled is E_INVALIDARG;
    // anything else needs developer mode (Learn) and is
    // DXGI_ERROR_SDK_COMPONENT_MISSING. There is no developer mode here, so
    // no cache is ever disabled.
    if (kinds == 0 ||
        (control != D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE &&
         control != D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE &&
         control != D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR)) {
        return E_INVALIDARG;
    }
    if (control == D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE) return E_INVALIDARG;
    return DXGI_ERROR_SDK_COMPONENT_MISSING;
}

u32 Frontend::set_background_processing_mode(u32 mode, u32 action,
                                             GuestAddr event,
                                             GuestAddr further) {
    // Measured (a11 control): mode or action out of range E_INVALIDARG;
    // DISABLE_BACKGROUND_WORK, DISABLE_PROFILING_BY_SYSTEM and
    // COMMIT_RESULTS_HIGH_PRIORITY need developer mode
    // (DXGI_ERROR_SDK_COMPONENT_MISSING); an event with any action but
    // COMMIT_RESULTS E_INVALIDARG. Then S_OK: there is no background shader
    // work to wait for, so the event is set at once and, as WARP answers,
    // *pbFurtherMeasurementsDesired is FALSE when an event is given and
    // untouched otherwise (NVIDIA writes TRUE: a driver's answer).
    if (mode > D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_PROFILING_BY_SYSTEM ||
        action > D3D12_MEASUREMENTS_ACTION_DISCARD_PREVIOUS) {
        return E_INVALIDARG;
    }
    if (mode == D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_BACKGROUND_WORK ||
        mode == D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_PROFILING_BY_SYSTEM ||
        action == D3D12_MEASUREMENTS_ACTION_COMMIT_RESULTS_HIGH_PRIORITY) {
        return DXGI_ERROR_SDK_COMPONENT_MISSING;
    }
    if (event != 0 && action != D3D12_MEASUREMENTS_ACTION_COMMIT_RESULTS) {
        return E_INVALIDARG;
    }
    if (event != 0) {
        if (further != 0) (void)m_memory.write_u32(further, 0);
        GpuEventSignaler::instance().signal_now(event);
    }
    return S_OK;
}

}  // namespace vitreo::winapi::d3d12
