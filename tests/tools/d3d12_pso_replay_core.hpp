// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d12_pso_replay_core.hpp
/// @brief Replays a VITREO_D3D12_DUMP_PSOS (or VITREO_D3D12_DUMP_REFUSED)
///        directory through the real D3D12 frontend and Metal backend, and
///        times each pipeline: its Create*PipelineState call, split by phase
///        (graphics/shaders/compile_profile.hpp), and the Metal render
///        pipeline its first draw forms.
///
/// A dump's `.mem.bin` holds every guest range the creation read, at its
/// guest address. Each pipeline's ranges are copied into its own slice of
/// one guest arena and the pointers in the description, stream, input
/// layout and stream-output declarations relocated, so pipelines replay on
/// several threads at once; the root signature is created again from its
/// blob. A dump without `.mem.bin` (an older VITREO_D3D12_DUMP_REFUSED one)
/// is a stream whose shader blobs are matched to its bytecode subobjects by
/// size, and whose input layout is rebuilt from the vertex shader's input
/// signature (one 4 x 32-bit element per input, slot 0).
///
/// x64 guests only; an x86 dump is counted and skipped.
///
/// Used by tests/tools/d3d12_pso_replay.cpp (the benchmark) and
/// tests/winapi/d3d12/test_d3d12_pso_replay.cpp (dump -> replay round trip).

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_vtables.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/compile_profile.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxil_container.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi::pso_replay {

using namespace d3d12;
namespace cp = graphics::compile_profile;

struct Region {
    GuestAddr at{0};
    u32 kind{0};  // 1: holds guest pointers (relocated)
    std::vector<u8> bytes;
};

struct Dump {
    std::string name;
    std::string api;
    bool x64{true};
    GuestAddr desc{0};
    u64 desc_size{0};
    GuestAddr stream{0};
    u64 stream_size{0};
    GuestAddr root{0};
    std::vector<u8> root_blob;
    std::vector<Region> regions;
    /// Stage ("vs", "ps", ...) and bytecode, from the per-stage files.
    std::vector<std::pair<std::string, std::vector<u8>>> blobs;
    bool legacy{false};
};

inline std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
}

inline bool decode_regions(const std::vector<u8>& mem, std::vector<Region>& out) {
    if (mem.size() < 16 || std::memcmp(mem.data(), "VPSOMEM1", 8) != 0) return false;
    u32 count = 0;
    std::memcpy(&count, mem.data() + 8, 4);
    usize at = 16;
    for (u32 i = 0; i < count; ++i) {
        if (at + 24 > mem.size()) return false;
        Region r;
        u64 size = 0;
        std::memcpy(&r.at, mem.data() + at, 8);
        std::memcpy(&size, mem.data() + at + 8, 8);
        std::memcpy(&r.kind, mem.data() + at + 16, 4);
        at += 24;
        if (at + size > mem.size()) return false;
        r.bytes.assign(mem.begin() + static_cast<std::ptrdiff_t>(at),
                       mem.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
        out.push_back(std::move(r));
    }
    return true;
}

/// Every `pso-<id>.reason.txt` of @p dir with its files.
inline std::vector<Dump> load_dumps(const std::filesystem::path& dir, std::string& error) {
    namespace fs = std::filesystem;
    std::vector<Dump> dumps;
    std::error_code ec;
    std::vector<fs::path> reasons;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string file = entry.path().filename().string();
        if (file.starts_with("pso-") && file.ends_with(".reason.txt")) {
            reasons.push_back(entry.path());
        }
    }
    if (ec) {
        error = "cannot list " + dir.string();
        return dumps;
    }
    std::sort(reasons.begin(), reasons.end());
    for (const fs::path& reason : reasons) {
        Dump d;
        const std::string file = reason.filename().string();
        d.name = file.substr(0, file.size() - std::strlen(".reason.txt"));
        std::ifstream text(reason);
        std::string line;
        while (std::getline(text, line)) {
            const auto value = [&](const char* key) -> const char* {
                const usize n = std::strlen(key);
                return line.compare(0, n, key) == 0 ? line.c_str() + n : nullptr;
            };
            if (const char* v = value("api: ")) d.api = v;
            if (const char* v = value("guest: ")) d.x64 = std::strcmp(v, "x64") == 0;
            if (const char* v = value("desc: ")) {
                d.desc = std::strtoull(v, nullptr, 16);
                if (const char* p = std::strchr(v, '(')) d.desc_size = std::strtoull(p + 1, nullptr, 10);
            }
            if (const char* v = value("stream: ")) {
                d.stream = std::strtoull(v, nullptr, 16);
                if (const char* p = std::strchr(v, '(')) d.stream_size = std::strtoull(p + 1, nullptr, 10);
            }
            if (const char* v = value("root: ")) d.root = std::strtoull(v, nullptr, 16);
        }
        const std::string prefix = d.name + ".";
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            const std::string f = entry.path().filename().string();
            if (!f.starts_with(prefix)) continue;
            const std::string rest = f.substr(prefix.size());
            if (rest.starts_with("rootsig-")) {
                d.root_blob = read_file(entry.path());
            } else if (rest == "mem.bin") {
                if (!decode_regions(read_file(entry.path()), d.regions)) d.regions.clear();
            } else if (rest.size() > 3 && rest[2] == '-' &&
                       (rest.ends_with(".dxbc") || rest.ends_with(".dxil"))) {
                d.blobs.emplace_back(rest.substr(0, 2), read_file(entry.path()));
            }
        }
        if (d.regions.empty()) {
            d.legacy = true;
            const auto desc = read_file(dir / (d.name + ".desc.bin"));
            const auto stream = read_file(dir / (d.name + ".stream.bin"));
            if (!desc.empty()) d.regions.push_back({d.desc, 1u, desc});
            if (!stream.empty()) d.regions.push_back({d.stream, 1u, stream});
        }
        dumps.push_back(std::move(d));
    }
    return dumps;
}

/// One guest thread's call frame.
struct Frame {
    cpu::CpuState cpu;
    GuestAddr base{0};
    [[nodiscard]] GuestAddr stack() const { return base + 0x3F000; }
    [[nodiscard]] GuestAddr iid() const { return base + 0x100; }
    [[nodiscard]] GuestAddr out() const { return base + 0x200; }
};

/// The exports and vtables of one x64 D3D12Manager (the shape of the
/// D3D12 tests' harnesses).
class Harness {
public:
    static constexpr usize kFrameBytes = 0x40000;
    static constexpr usize kFrames = 65;

    Harness() {
        frames_base = memory.map_first_fit(0x100000, kFrameBytes * kFrames,
                                           MemoryProtection::ReadWrite);
        manager = std::make_unique<D3D12Manager>(memory, true,
                                                 graphics::MetalDevice::instance());
        exports = create_d3d12_thunks(*manager);
        for (const auto& entry : exports.entries) by_name[entry.name] = &entry;
        D3D12Manager::MethodThunks thunks{};
        GuestAddr next = 0x70000000;
        const auto fill = [&](const char* interface_name, auto& table) {
            for (usize slot = 0; slot < table.size(); ++slot) {
                table[slot] = next;
                by_thunk[next] = d3d12_method_export_name(interface_name, slot);
                next += 0x10;
            }
        };
        for (usize table = 0; table < INTERFACE_TABLE_COUNT; ++table) {
            thunks.tables[table].resize(INTERFACE_TABLES[table].method_count);
            fill(INTERFACE_TABLES[table].export_name, thunks.tables[table]);
        }
        fill("Blob", thunks.blob);
        fill("RootSignatureDeserializer", thunks.root_signature_deserializer);
        fill("VersionedRootSignatureDeserializer", thunks.versioned_root_signature_deserializer);
        manager->set_method_thunks(thunks);
    }

    [[nodiscard]] Frame frame(usize slot) const {
        Frame f;
        f.base = frames_base + slot * kFrameBytes;
        f.cpu.mode = cpu::CpuMode::Long;
        return f;
    }

    u64 call(Frame& f, const std::string& name, const std::vector<u64>& args) {
        const auto found = by_name.find(name);
        if (found == by_name.end()) return ~0ull;
        f.cpu.gpr.rsp = f.stack();
        f.cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        u64* regs[] = {&f.cpu.gpr.rcx, &f.cpu.gpr.rdx, &f.cpu.gpr.r8, &f.cpu.gpr.r9};
        for (usize i = 0; i < args.size(); ++i) {
            if (i < 4) {
                *regs[i] = args[i];
            } else {
                (void)memory.write_u64(f.stack() + 0x28 + (i - 4) * 8, args[i]);
            }
        }
        Marshaler m(memory);
        m.attach_cpu(f.cpu);
        found->second->handler(m);
        return f.cpu.gpr.rax;
    }

    u64 com(Frame& f, GuestAddr object, usize slot, std::vector<u64> args) {
        const u64 vtable = memory.read_u64(object).value();
        const u64 thunk = memory.read_u64(vtable + slot * 8).value();
        const auto name = by_thunk.find(thunk);
        if (name == by_thunk.end()) return ~0ull;
        args.insert(args.begin(), object);
        return call(f, name->second, args);
    }

    GuestAddr iid(Frame& f, const GuidBytes& value) {
        (void)memory.write_bytes(f.iid(), value.data(), value.size());
        return f.iid();
    }

    GuestAddr made(Frame& f, u64 hr) {
        return hr == S_OK ? memory.read_u64(f.out()).value() : 0;
    }

    GuestMemory memory;
    GuestAddr frames_base{0};
    std::unique_ptr<D3D12Manager> manager;
    DllExports exports;
    std::unordered_map<std::string, const ExportEntry*> by_name;
    std::unordered_map<u64, std::string> by_thunk;
    GuestAddr device{0};
};

/// One pipeline's replay.
struct PsoResult {
    std::string name;
    std::string api;
    u32 hr{0};
    bool skipped{false};
    std::string skip_reason;
    u64 create_ns{0};
    u64 draw_ns{0};
    /// Phase totals over the call (exact on one thread only).
    cp::Snapshot phases_before{};
    cp::Snapshot phases_after{};
    GuestAddr pso{0};
};

struct RunResult {
    std::vector<PsoResult> pipelines;
    u64 create_wall_ns{0};
    u64 draw_wall_ns{0};
    cp::Snapshot before{};
    cp::Snapshot after_create{};
    cp::Snapshot after_draw{};
};

/// Prepares and replays dumps on one device.
class Replayer {
public:
    explicit Replayer(std::string& error) {
        if (!graphics::MetalDevice::instance().available()) {
            error = "no Metal device";
            return;
        }
        ::setenv("VITREO_D3D12", "1", 1);
        h = std::make_unique<Harness>();
        if (h->frames_base == 0) {
            error = "cannot map the call frames";
            return;
        }
        Frame f = h->frame(0);
        h->device = h->made(f, h->call(f, "D3D12CreateDevice",
                                       {0, D3D_FEATURE_LEVEL_11_0,
                                        h->iid(f, IID_ID3D12DEVICE2), f.out()}));
        if (h->device == 0) error = "D3D12CreateDevice failed";
    }

    /// Lay every dump out in the arena (relocated). false: @p error.
    bool prepare(const std::vector<Dump>& dumps, std::string& error) {
        u64 total = 0x10000;
        for (const Dump& d : dumps) {
            for (const Region& r : d.regions) total += (r.bytes.size() + 31) & ~u64{15};
            for (const auto& [stage, bytes] : d.blobs) total += (bytes.size() + 31) & ~u64{15};
            total += 0x2000;  // a rebuilt input layout
        }
        total = (total + 0xFFFF) & ~u64{0xFFFF};
        arena = h->memory.map_first_fit(0x10000000, total, MemoryProtection::ReadWrite);
        if (arena == 0) {
            error = "cannot map a " + std::to_string(total) + "-byte arena";
            return false;
        }
        arena_cursor = arena;
        arena_end = arena + total;
        prepared.clear();
        for (const Dump& d : dumps) prepared.push_back(lay_out(d));
        return true;
    }

    /// Create every prepared pipeline on @p threads threads, then (with
    /// @p draw) form each graphics pipeline's first-draw Metal pipeline on
    /// this thread, as the queue thread does. @p release drops each pipeline
    /// at the end.
    RunResult run(unsigned threads, bool draw, bool release = true) {
        RunResult result;
        result.pipelines.resize(prepared.size());
        threads = std::max(1u, std::min<unsigned>(threads, Harness::kFrames - 1));
        cp::reset_max_in_flight();
        result.before = cp::snapshot();
        std::atomic<usize> next{0};
        const u64 t0 = cp::now_ns();
        const auto worker = [&](unsigned slot) {
            Frame f = h->frame(slot + 1);
            for (;;) {
                const usize i = next.fetch_add(1);
                if (i >= prepared.size()) return;
                create_one(f, prepared[i], result.pipelines[i]);
            }
        };
        if (threads == 1) {
            worker(0);
        } else {
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker, t);
            for (auto& thread : pool) thread.join();
        }
        result.create_wall_ns = cp::now_ns() - t0;
        result.after_create = cp::snapshot();
        const u64 t1 = cp::now_ns();
        if (draw) {
            for (PsoResult& r : result.pipelines) {
                if (r.pso != 0) draw_one(r);
            }
        }
        result.draw_wall_ns = cp::now_ns() - t1;
        result.after_draw = cp::snapshot();
        if (release) {
            Frame f = h->frame(0);
            for (PsoResult& r : result.pipelines) {
                if (r.pso != 0) (void)h->com(f, r.pso, DeviceSlot::Release, {});
                r.pso = 0;
            }
        }
        return result;
    }

    std::unique_ptr<Harness> h;

private:
    struct Prepared {
        std::string name;
        std::string api;
        GuestAddr desc{0};
        bool stream_api{false};
        std::string skip;
        /// Where the old root pointer is written, once the root is made.
        std::vector<GuestAddr> root_slots;
        const std::vector<u8>* root_blob{nullptr};
    };

    GuestAddr place(const std::vector<u8>& bytes) {
        const GuestAddr at = arena_cursor;
        arena_cursor = (arena_cursor + bytes.size() + 15) & ~GuestAddr{15};
        if (arena_cursor > arena_end || bytes.empty()) return at;
        (void)h->memory.write_bytes(at, bytes.data(), bytes.size());
        return at;
    }

    Prepared lay_out(const Dump& d) {
        Prepared p;
        p.name = d.name;
        p.api = d.api;
        p.root_blob = &d.root_blob;
        if (!d.x64) {
            p.skip = "x86 guest";
            return p;
        }
        p.stream_api = d.api.find("CreatePipelineState") != std::string::npos &&
                       d.api.find("Graphics") == std::string::npos &&
                       d.api.find("Compute") == std::string::npos;
        if (d.api.find("CreateGraphicsPipelineState") == std::string::npos &&
            d.api.find("CreateComputePipelineState") == std::string::npos && !p.stream_api) {
            p.skip = "not a Create*PipelineState dump (" + d.api + ")";
            return p;
        }
        std::vector<Region> regions = d.regions;
        GuestAddr old_root = d.root;
        if (d.legacy && !legacy_regions(d, regions, old_root, p.skip)) return p;
        // Copy, then relocate the pointers in the pointer-holding ranges.
        std::vector<GuestAddr> moved(regions.size());
        for (usize i = 0; i < regions.size(); ++i) moved[i] = place(regions[i].bytes);
        if (arena_cursor > arena_end) {
            p.skip = "arena overflow";
            return p;
        }
        const auto relocate = [&](u64 value, u64& out) {
            for (usize i = 0; i < regions.size(); ++i) {
                const Region& r = regions[i];
                if (value >= r.at && value < r.at + r.bytes.size()) {
                    out = moved[i] + (value - r.at);
                    return true;
                }
            }
            return false;
        };
        for (usize i = 0; i < regions.size(); ++i) {
            if (regions[i].kind != 1u) continue;
            for (usize off = 0; off + 8 <= regions[i].bytes.size(); off += 8) {
                u64 value = 0;
                std::memcpy(&value, regions[i].bytes.data() + off, 8);
                if (value == 0) continue;
                u64 target = 0;
                if (old_root != 0 && value == old_root) {
                    p.root_slots.push_back(moved[i] + off);
                } else if (relocate(value, target)) {
                    (void)h->memory.write_u64(moved[i] + off, target);
                }
            }
        }
        u64 desc = 0;
        if (!relocate(d.desc, desc)) {
            p.skip = "the description is not in the dump";
            return p;
        }
        p.desc = desc;
        return p;
    }

    /// An older dump: no `.mem.bin`. Map each stage blob where the stream's
    /// bytecode subobject points, and rebuild a missing input layout.
    bool legacy_regions(const Dump& d, std::vector<Region>& regions, GuestAddr& old_root,
                        std::string& skip) {
        if (d.stream == 0) {
            skip = "an old dump of a description (no .mem.bin)";
            return false;
        }
        const Region* stream = nullptr;
        for (const Region& r : regions) {
            if (r.at == d.stream) stream = &r;
        }
        if (stream == nullptr) {
            skip = "no stream bytes";
            return false;
        }
        const std::vector<u8> s = stream->bytes;  // regions grows below
        const auto u32_at = [&](usize at) {
            u32 v = 0;
            if (at + 4 <= s.size()) std::memcpy(&v, s.data() + at, 4);
            return v;
        };
        const auto u64_at = [&](usize at) {
            u64 v = 0;
            if (at + 8 <= s.size()) std::memcpy(&v, s.data() + at, 8);
            return v;
        };
        const auto align = [](u64 v, u64 a) { return (v + a - 1) / a * a; };
        const std::vector<u8>* vs = nullptr;
        u64 cursor = 0;
        while (cursor + 4 <= s.size()) {
            const u32 type = u32_at(cursor);
            u64 inner_align = 4;
            u64 inner_size = 4;
            const char* stage = nullptr;
            switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: inner_align = inner_size = 8; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: stage = "vs"; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: stage = "ps"; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: stage = "ds"; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: stage = "hs"; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: stage = "gs"; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: stage = "cs"; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: stage = "as"; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: stage = "ms"; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
                inner_align = 8; inner_size = layout::D3D12_STREAM_OUTPUT_DESC[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
                inner_size = layout::D3D12_BLEND_DESC[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
                inner_size = layout::D3D12_RASTERIZER_DESC[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
                inner_size = layout::D3D12_RASTERIZER_DESC1[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
                inner_size = layout::D3D12_RASTERIZER_DESC2[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
                inner_size = layout::D3D12_DEPTH_STENCIL_DESC[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
                inner_size = layout::D3D12_DEPTH_STENCIL_DESC1[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
                inner_size = layout::D3D12_DEPTH_STENCIL_DESC2[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
                inner_align = 8; inner_size = layout::D3D12_INPUT_LAYOUT_DESC[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
                inner_size = layout::D3D12_RT_FORMAT_ARRAY[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
                inner_size = layout::DXGI_SAMPLE_DESC[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
                inner_align = 8; inner_size = layout::D3D12_CACHED_PIPELINE_STATE[1].size; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
                inner_align = 8; inner_size = layout::D3D12_VIEW_INSTANCING_DESC[1].size; break;
            default:
                skip = "an old dump with stream subobject " + std::to_string(type);
                return false;
            }
            if (stage != nullptr) {
                inner_align = 8;
                inner_size = layout::D3D12_SHADER_BYTECODE[1].size;
            }
            const u64 at = align(cursor + 4, inner_align);
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) old_root = u64_at(at);
            if (stage != nullptr) {
                const u64 pointer = u64_at(at);
                const u64 length = u64_at(at + 8);
                for (const auto& [name, bytes] : d.blobs) {
                    if (name == stage && bytes.size() == length && pointer != 0) {
                        regions.push_back({pointer, 0u, bytes});
                        if (name == "vs") vs = &bytes;
                    }
                }
            }
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
                legacy_input_layout_at = d.stream + at;
            }
            cursor = align(at + inner_size, 8);
        }
        // The input layout: one element per VS input register.
        if (legacy_input_layout_at != 0) {
            const u64 off = legacy_input_layout_at - d.stream;
            legacy_input_layout_at = 0;
            const u64 elements = u64_at(off);
            const u32 count = u32_at(off + 8);
            if (elements != 0 && count != 0 && vs != nullptr) {
                const auto container = graphics::parse_dxbc(vs->data(), vs->size());
                std::vector<u8> table;
                std::vector<u8> names;
                std::vector<std::pair<usize, usize>> fixups;  // element, name offset
                const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[1];
                u32 n = 0;
                for (const auto& in : container.input_signature) {
                    if (in.system_value_type != 0) continue;
                    std::vector<u8> e(il.size, 0);
                    const u32 format = in.component_type == 1 ? DXGI_FORMAT_R32G32B32A32_UINT
                                       : in.component_type == 2 ? DXGI_FORMAT_R32G32B32A32_SINT
                                                                : DXGI_FORMAT_R32G32B32A32_FLOAT;
                    const u32 offset = in.register_num * 16u;
                    std::memcpy(e.data() + il.SemanticIndex, &in.semantic_index, 4);
                    std::memcpy(e.data() + il.Format, &format, 4);
                    std::memcpy(e.data() + il.AlignedByteOffset, &offset, 4);
                    fixups.emplace_back(table.size(), names.size());
                    names.insert(names.end(), in.semantic_name.begin(), in.semantic_name.end());
                    names.push_back(0);
                    table.insert(table.end(), e.begin(), e.end());
                    ++n;
                }
                // Synthetic guest addresses past every real range; relocate()
                // maps them like any other.
                const u64 table_at = 0xFFFF00000000ull;
                const u64 names_at = 0xFFFF10000000ull;
                for (const auto& [element, name] : fixups) {
                    const u64 pointer = names_at + name;
                    std::memcpy(table.data() + element + il.SemanticName, &pointer, 8);
                }
                for (Region& r : regions) {
                    if (r.at != d.stream) continue;
                    const u64 at = n != 0 ? table_at : 0;
                    std::memcpy(r.bytes.data() + off, &at, 8);
                    std::memcpy(r.bytes.data() + off + 8, &n, 4);
                }
                if (n != 0) {
                    regions.push_back({table_at, 1u, std::move(table)});
                    regions.push_back({names_at, 0u, std::move(names)});
                }
            }
        }
        return true;
    }

    GuestAddr root_for(Frame& f, const std::vector<u8>& blob) {
        if (blob.empty()) return 0;
        const u64 key = fnv1a(blob.data(), blob.size());
        {
            std::lock_guard lock(root_mutex);
            if (auto it = roots.find(key); it != roots.end()) return it->second;
        }
        const GuestAddr scratch = f.base + 0x1000;
        if (blob.size() > 0x30000) return 0;
        (void)h->memory.write_bytes(scratch, blob.data(), blob.size());
        const GuestAddr root =
            h->made(f, h->com(f, h->device, DeviceSlot::CreateRootSignature,
                              {0, scratch, blob.size(), h->iid(f, IID_ID3D12ROOTSIGNATURE),
                               f.out()}));
        std::lock_guard lock(root_mutex);
        return roots.emplace(key, root).first->second;
    }

    void create_one(Frame& f, const Prepared& p, PsoResult& r) {
        r.name = p.name;
        r.api = p.api;
        if (!p.skip.empty()) {
            r.skipped = true;
            r.skip_reason = p.skip;
            return;
        }
        const GuestAddr root = root_for(f, *p.root_blob);
        for (GuestAddr slot : p.root_slots) (void)h->memory.write_u64(slot, root);
        const usize method = p.stream_api ? DeviceSlot::CreatePipelineState
                             : p.api.find("Compute") != std::string::npos
                                 ? DeviceSlot::CreateComputePipelineState
                                 : DeviceSlot::CreateGraphicsPipelineState;
        r.phases_before = cp::snapshot();
        const u64 t0 = cp::now_ns();
        const u64 hr = h->com(f, h->device, method,
                              {p.desc, h->iid(f, IID_ID3D12PIPELINESTATE), f.out()});
        r.create_ns = cp::now_ns() - t0;
        r.phases_after = cp::snapshot();
        r.hr = static_cast<u32>(hr);
        r.pso = h->made(f, hr);
    }

    void draw_one(PsoResult& r) {
        Frontend& frontend = h->manager->frontend();
        FrontendLock lock(frontend);
        PipelineState* pso = frontend.find<PipelineState>(r.pso, Kind::PipelineState);
        // The pipelines a plain draw forms through pipeline_variant (the
        // test of Frontend::precompile_draw_functions); the lowered stages
        // build theirs elsewhere.
        if (pso == nullptr || pso->compute || pso->mesh.active || pso->geometry.active ||
            pso->tessellation.active || pso->raster.active || pso->stream_output.active ||
            pso->vertex_cull.active || !pso->per_vertex_msl.empty() ||
            pso->view_instance_count > 1) {
            return;
        }
        // The strides a draw would bind: each stream as wide as the layout
        // reads it.
        std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS> strides{};
        for (const InputElement& e : pso->graphics.input_layout) {
            if (e.input_slot >= pso->graphics.slot_stream.size()) continue;
            const u32 stream = pso->graphics.slot_stream[e.input_slot];
            if (stream >= strides.size() || e.aligned_offset == 0xFFFFFFFFu) continue;
            strides[stream] = std::max<u32>(
                strides[stream],
                e.aligned_offset + d3d11::vertex_format_stride(d3d11::vertex_format_from_dxgi(e.format)));
        }
        const u64 t0 = cp::now_ns();
        (void)frontend.pipeline_variant(*pso, strides);
        r.draw_ns = cp::now_ns() - t0;
    }

    std::vector<Prepared> prepared;
    GuestAddr arena{0};
    GuestAddr arena_cursor{0};
    GuestAddr arena_end{0};
    u64 legacy_input_layout_at{0};
    std::mutex root_mutex;
    std::map<u64, GuestAddr> roots;
};

}  // namespace vitreo::winapi::pso_replay
