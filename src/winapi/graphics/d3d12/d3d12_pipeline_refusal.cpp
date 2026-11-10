// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_pipeline_refusal.cpp
/// @brief Why a D3D12 pipeline creation failed: the per-thread reason record
/// every refusal path writes (d3d12_diag, refuse, unimplemented), the one
/// VITREO_D3D12_DIAG line a failed Create*PipelineState call ends with, and
/// the VITREO_D3D12_DUMP_REFUSED dump of the refused description and its
/// shader blobs. Diagnostic only: nothing here changes a result.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "graphics/shaders/dxil_container.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vitreo::winapi::d3d12 {

namespace {

struct RefusalLog {
    u32 depth{0};
    std::string reasons;
};

RefusalLog& refusal_log() {
    thread_local RefusalLog log;
    return log;
}

/// The bytes of guest memory [address, address + size), or empty.
std::vector<u8> read_guest(GuestMemory& memory, GuestAddr address, u64 size) {
    std::vector<u8> bytes;
    if (address == 0 || size == 0 || size > (256ull << 20)) return bytes;
    bytes.resize(static_cast<usize>(size));
    if (memory.read_bytes(address, bytes.data(), bytes.size()).is_err()) {
        bytes.clear();
    }
    return bytes;
}

bool write_file(const std::filesystem::path& path, const void* data, usize size) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(file);
}

std::string hex64(u64 value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

}  // namespace

const char* refused_pipeline_dump_dir() {
    // Read at each refusal (they are rare), not cached at the first.
    const char* value = std::getenv("VITREO_D3D12_DUMP_REFUSED");
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

const char* created_pipeline_dump_dir() {
    // Read at each creation: a getenv is nothing beside a pipeline build.
    const char* value = std::getenv("VITREO_D3D12_DUMP_PSOS");
    if (value != nullptr && value[0] != '\0') return value;
    // A store-launched guest does not reliably inherit the environment: the
    // marker file /tmp/vitreo-d3d12-dump-psos (read once) names the
    // directory on its first line, or /tmp/vitreo-d3d12-psos when empty.
    static const std::string marker = [] {
        std::ifstream file("/tmp/vitreo-d3d12-dump-psos");
        if (!file) return std::string();
        std::string dir;
        std::getline(file, dir);
        while (!dir.empty() && (dir.back() == ' ' || dir.back() == '\r')) dir.pop_back();
        return dir.empty() ? std::string("/tmp/vitreo-d3d12-psos") : dir;
    }();
    return marker.empty() ? nullptr : marker.c_str();
}

bool pipeline_refusal_recording() { return refusal_log().depth != 0; }

void note_pipeline_refusal(const char* text) {
    RefusalLog& log = refusal_log();
    if (log.depth == 0 || text == nullptr) return;
    // Bounded: a pathological shader cannot grow the record without end.
    if (log.reasons.size() > 16384) return;
    if (!log.reasons.empty()) log.reasons += " | ";
    log.reasons += text;
}

u32 refuse(u32 hr, const char* format, ...) {
    if (!diag_enabled() && !pipeline_refusal_recording()) return hr;
    char buffer[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    d3d12_diag("%s -> 0x%08x", buffer, hr);
    return hr;
}

Frontend::PipelineRefusalScope::PipelineRefusalScope(Frontend& frontend,
                                                     Marshaler& m,
                                                     const char* api)
    : m_frontend(frontend), m_m(m), m_api(api) {
    RefusalLog& log = refusal_log();
    m_saved = std::move(log.reasons);
    log.reasons.clear();
    ++log.depth;
}

Frontend::PipelineRefusalScope::~PipelineRefusalScope() {
    RefusalLog& log = refusal_log();
    std::string reasons = std::move(log.reasons);
    --log.depth;
    log.reasons = std::move(m_saved);
    const u32 hr = static_cast<u32>(m_m.return_value());
    if ((hr & 0x80000000u) == 0) {
        if (const char* dir = created_pipeline_dump_dir(); dir != nullptr) {
            dump(hr, "created", dir);
        }
        return;
    }
    if (reasons.empty()) {
        // Every refusal path records its reason; one that did not is a gap
        // in this diagnostic, named as such rather than left silent.
        reasons = "no refusal reason was recorded on this path";
    }
    if (diag_enabled()) {
        std::fprintf(stderr, "[d3d12-diag] %s: refused 0x%08x: %s\n", m_api, hr,
                     reasons.c_str());
    }
    // An enclosing scope (a pipeline library's Load*) sees this one's reasons.
    if (log.depth != 0) note_pipeline_refusal(reasons.c_str());
    if (const char* dir = refused_pipeline_dump_dir(); dir != nullptr) {
        dump(hr, reasons, dir);
    }
}

void Frontend::PipelineRefusalScope::dump(u32 hr, const std::string& reasons,
                                          const char* dir_name) {
    namespace fs = std::filesystem;
    GuestMemory& memory = m_frontend.m_memory;
    const bool x64 = m_frontend.m_is_64bit;
    const std::vector<u8> desc_bytes = read_guest(memory, desc, desc_size);
    const std::vector<u8> stream_bytes = read_guest(memory, stream, stream_size);
    struct Blob {
        const char* stage;
        std::vector<u8> bytes;
    };
    std::vector<Blob> blobs;
    std::vector<u8> root_blob;
    if (src != nullptr) {
        const std::pair<const char*, GuestAddr> stages[] = {
            {"vs", src->vs}, {"ps", src->ps}, {"ds", src->ds}, {"hs", src->hs},
            {"gs", src->gs}, {"cs", src->cs}, {"as", src->as}, {"ms", src->ms}};
        const auto& l = layout::D3D12_SHADER_BYTECODE[x64];
        for (const auto& [stage, at] : stages) {
            if (at == 0) continue;
            GuestAddr pointer = 0;
            GuestAddr length = 0;
            if (!m_frontend.read_pointer(at + l.pShaderBytecode, pointer) ||
                !m_frontend.read_pointer(at + l.BytecodeLength, length)) {
                continue;
            }
            std::vector<u8> bytes = read_guest(memory, pointer, length);
            if (!bytes.empty()) blobs.push_back({stage, std::move(bytes)});
        }
        // The creator holds the frontend lock again by now (phase (c) of
        // build_pipeline_unlocked).
        if (src->root != 0) {
            if (const RootSignature* root = m_frontend.find<RootSignature>(
                    src->root, Kind::RootSignature)) {
                root_blob = root->blob;
            }
        }
    }
    // Every guest range the creation read, at its guest address, so a
    // replay (tests/tools/d3d12_pso_replay.cpp) can map them again and make
    // the same call: the description and stream (kRegionPointers: the
    // replay relocates the pointers in them), the shader bytecode, the input
    // layout and stream-output declarations with their semantic names, the
    // stream-output strides, the view instance locations, the cached blob.
    std::vector<Region> regions;
    collect_regions(desc_bytes, stream_bytes, regions);
    std::vector<u8> mem;
    if (!regions.empty()) mem = encode_regions(regions);
    u64 id = fnv1a(desc_bytes.data(), desc_bytes.size());
    id = fnv1a(stream_bytes.data(), stream_bytes.size(), id);
    for (const Blob& blob : blobs) id = fnv1a(blob.bytes.data(), blob.bytes.size(), id);
    id = fnv1a(root_blob.data(), root_blob.size(), id);
    for (const Region& region : regions) {
        if (region.kind != kRegionPointers) {
            id = fnv1a(region.bytes.data(), region.bytes.size(), id);
        }
    }
    std::error_code ec;
    const fs::path dir(dir_name);
    fs::create_directories(dir, ec);
    const std::string prefix = (dir / ("pso-" + hex64(id))).string();
    // Once per distinct pipeline and directory.
    static std::mutex mutex;
    static std::set<std::pair<std::string, u64>> dumped;
    {
        std::lock_guard lock(mutex);
        if (!dumped.insert({dir.string(), id}).second) return;
    }
    std::string listing;
    const auto put = [&](const std::string& suffix, const std::vector<u8>& bytes) {
        if (bytes.empty()) return;
        const std::string path = prefix + suffix;
        if (write_file(path, bytes.data(), bytes.size())) {
            listing += "  " + fs::path(path).filename().string() + " (" +
                       std::to_string(bytes.size()) + " bytes)\n";
        }
    };
    put(".desc.bin", desc_bytes);
    put(".stream.bin", stream_bytes);
    put(".mem.bin", mem);
    put(".rootsig-" + hex64(fnv1a(root_blob.data(), root_blob.size())) + ".bin", root_blob);
    for (const Blob& blob : blobs) {
        const bool dxil = graphics::dxil_container_present(blob.bytes.data(), blob.bytes.size());
        put(std::string(".") + blob.stage + "-" +
                hex64(fnv1a(blob.bytes.data(), blob.bytes.size())) +
                (dxil ? ".dxil" : ".dxbc"),
            blob.bytes);
    }
    char head[384];
    std::snprintf(head, sizeof(head),
                  "api: %s\nhr: 0x%08x\nguest: %s\ndesc: 0x%llx (%llu bytes)\n"
                  "stream: 0x%llx (%llu bytes)\nroot: 0x%llx\n",
                  m_api, hr, x64 ? "x64" : "x86",
                  static_cast<unsigned long long>(desc),
                  static_cast<unsigned long long>(desc_size),
                  static_cast<unsigned long long>(stream),
                  static_cast<unsigned long long>(stream_size),
                  static_cast<unsigned long long>(src != nullptr ? src->root : 0));
    const std::string text =
        std::string(head) + "reasons: " + reasons + "\nfiles:\n" + listing;
    if (write_file(prefix + ".reason.txt", text.data(), text.size()) && diag_enabled()) {
        std::fprintf(stderr, "[d3d12-diag] %s: %s pipeline dumped to %s.*\n", m_api,
                     (hr & 0x80000000u) != 0 ? "refused" : "created", prefix.c_str());
    }
}

void Frontend::PipelineRefusalScope::collect_regions(const std::vector<u8>& desc_bytes,
                                                     const std::vector<u8>& stream_bytes,
                                                     std::vector<Region>& regions) const {
    GuestMemory& memory = m_frontend.m_memory;
    const bool x64 = m_frontend.m_is_64bit;
    const auto add = [&](GuestAddr at, std::vector<u8> bytes, u32 kind) {
        if (at == 0 || bytes.empty()) return;
        regions.push_back({at, kind, std::move(bytes)});
    };
    const auto add_string = [&](GuestAddr at) {
        if (at == 0) return;
        const auto text = m_m.read_ansi_string(at, 256);
        if (!text.is_ok()) return;
        std::vector<u8> bytes(text.value().begin(), text.value().end());
        bytes.push_back(0);
        add(at, std::move(bytes), kRegionData);
    };
    add(desc, desc_bytes, kRegionPointers);
    add(stream, stream_bytes, kRegionPointers);
    if (src == nullptr) return;
    const auto& l = layout::D3D12_SHADER_BYTECODE[x64];
    for (const GuestAddr at : {src->vs, src->ps, src->ds, src->hs, src->gs, src->cs,
                               src->as, src->ms}) {
        if (at == 0) continue;
        GuestAddr pointer = 0;
        GuestAddr length = 0;
        if (!m_frontend.read_pointer(at + l.pShaderBytecode, pointer) ||
            !m_frontend.read_pointer(at + l.BytecodeLength, length)) {
            continue;
        }
        add(pointer, read_guest(memory, pointer, length), kRegionData);
    }
    if (input_elements != 0 && input_element_count != 0) {
        const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[x64];
        add(input_elements,
            read_guest(memory, input_elements, u64{input_element_count} * il.size),
            kRegionPointers);
        for (u32 index = 0; index < input_element_count; ++index) {
            GuestAddr name = 0;
            if (m_frontend.read_pointer(input_elements + index * il.size + il.SemanticName,
                                        name)) {
                add_string(name);
            }
        }
    }
    if (src->so_declaration != 0 && src->stream_output_entries != 0) {
        const auto& so = layout::D3D12_SO_DECLARATION_ENTRY[x64];
        add(src->so_declaration,
            read_guest(memory, src->so_declaration,
                       u64{src->stream_output_entries} * so.size),
            kRegionPointers);
        for (u32 index = 0; index < src->stream_output_entries; ++index) {
            GuestAddr name = 0;
            if (m_frontend.read_pointer(
                    src->so_declaration + index * so.size + so.SemanticName, name)) {
                add_string(name);
            }
        }
    }
    if (src->so_strides != 0 && src->so_num_strides != 0) {
        add(src->so_strides,
            read_guest(memory, src->so_strides, u64{src->so_num_strides} * 4), kRegionData);
    }
    if (src->view_locations != 0 && src->view_instances != 0) {
        add(src->view_locations,
            read_guest(memory, src->view_locations,
                       u64{src->view_instances} *
                           layout::D3D12_VIEW_INSTANCE_LOCATION[x64].size),
            kRegionData);
    }
    if (src->cached_blob != 0 && src->cached_size != 0) {
        add(src->cached_blob, read_guest(memory, src->cached_blob, src->cached_size),
            kRegionData);
    }
}

std::vector<u8> Frontend::PipelineRefusalScope::encode_regions(
    const std::vector<Region>& regions) const {
    // "VPSOMEM1", u32 count, u32 pointer bytes; then per region u64 guest
    // address, u64 size, u32 kind, u32 0 and the bytes (little-endian).
    std::vector<u8> out;
    const auto put = [&out](const void* data, usize size) {
        const auto* p = static_cast<const u8*>(data);
        out.insert(out.end(), p, p + size);
    };
    put("VPSOMEM1", 8);
    const u32 count = static_cast<u32>(regions.size());
    const u32 pointer = m_frontend.m_is_64bit ? 8u : 4u;
    put(&count, 4);
    put(&pointer, 4);
    for (const Region& region : regions) {
        const u64 at = region.at;
        const u64 size = region.bytes.size();
        const u32 zero = 0;
        put(&at, 8);
        put(&size, 8);
        put(&region.kind, 4);
        put(&zero, 4);
        put(region.bytes.data(), region.bytes.size());
    }
    return out;
}

}  // namespace vitreo::winapi::d3d12
