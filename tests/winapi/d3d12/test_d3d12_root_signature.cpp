// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12_root_signature.cpp
/// @brief d3d12.dll root signatures, replayed against Windows.
///
/// tools/oracle_probe/d3d12_rootsig_probe.c ran on Windows (26100) and
/// printed every input it used next to Windows' answer:
/// d3d12_rootsig_probe{64,32}.expected. These tests rebuild each input from
/// those files alone -- the descs from the "in" lines, laid out in guest memory
/// by offsets read from the file's own [layout] lines, the blob mutations from
/// the "mutation" lines -- call the guest-visible exports and vtable slots,
/// print the result in the probe's format, and require the text to match line
/// for line. Nothing expected here comes from running Vitreo.
///
/// d3d12_rootsig_probe_device64.expected is ID3D12Device::CreateRootSignature
/// on the same inputs; it is replayed against parse_root_signature_blob_for_device,
/// the seam D12CORE's CreateRootSignature calls.

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {
namespace {

// =============================================================================
// Fixtures shared by every replay
// =============================================================================

/// The root-signature code never touches the backend; D3D12Manager needs one.
class NullBackend final : public graphics::GraphicsBackendDevice {
public:
    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] std::string backend_device_name() const override {
        return "Null";
    }
    [[nodiscard]] void* native_device() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] void* native_command_queue() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendBuffer> create_buffer(
        const graphics::BackendBufferDesc&, const void*) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendTexture2D>
    create_texture_2d(const graphics::BackendTexture2DDesc&, const void*,
                      unsigned) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendSwapChain> create_swap_chain(
        const graphics::BackendSwapChainDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendPipeline> create_pipeline(
        const graphics::BackendPipelineDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendComputePipeline>
    create_compute_pipeline(
        const graphics::BackendComputePipelineDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendRenderPass>
    create_render_pass_standalone(
        const graphics::BackendRenderPassDesc&) override {
        return nullptr;
    }
    [[nodiscard]] bool dispatch(const graphics::BackendDispatchCall&) override {
        return false;
    }
    [[nodiscard]] bool copy_buffer(graphics::BackendBuffer*,
                                   const graphics::BackendBuffer*) override {
        return false;
    }
    [[nodiscard]] bool copy_texture_2d(
        graphics::BackendTexture2D*,
        const graphics::BackendTexture2D*) override {
        return false;
    }
    [[nodiscard]] bool generate_mips(graphics::BackendTexture2D*) override {
        return false;
    }
    void flush() override {}
};

std::string fmt(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

std::vector<std::string> read_lines(const std::string& name) {
    std::ifstream file(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + name);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) lines.push_back(line);
    return lines;
}

/// The lines of one "== name" section, without the header.
std::vector<std::string> section(const std::vector<std::string>& lines,
                                 const std::string& name) {
    std::vector<std::string> out;
    bool inside = false;
    for (const std::string& line : lines) {
        if (line.rfind("== ", 0) == 0) {
            inside = line.rfind("== " + name, 0) == 0;
            continue;
        }
        if (inside) out.push_back(line);
    }
    return out;
}

/// key=value lookup in a space-separated line ("hr=0x.." -> "0x..").
std::string field(const std::string& line, const std::string& key) {
    std::istringstream in(line);
    std::string token;
    while (in >> token) {
        if (token.rfind(key + "=", 0) == 0) return token.substr(key.size() + 1);
    }
    return {};
}

bool has_token(const std::string& line, const std::string& token) {
    std::istringstream in(line);
    std::string t;
    while (in >> t) {
        if (t == token) return true;
    }
    return false;
}

u32 num(const std::string& text) {
    return static_cast<u32>(std::stoul(text, nullptr, 0));
}

/// Offsets of the SDK structures, from the probe's [layout] section.
struct ProbeLayout {
    bool is_64bit{true};
    u32 ptr{8};
    u32 param_size{0}, param_union{0}, param_ranges{0}, param_vis{0};
    u32 desc_size{0}, desc_params{0}, desc_nsamplers{0}, desc_samplers{0},
        desc_flags{0};
    u32 versioned_size{0}, versioned_desc{0};
    u32 range_size{0}, range1_size{0}, range1_flags{0}, range1_offset{0};
    u32 sampler_size{0}, sampler1_size{0}, sampler1_flags{0};
};

u32 at_offset(const std::string& line, const std::string& name) {
    const auto pos = line.find(" " + name + "@");
    return pos == std::string::npos ? 0 : num(line.substr(pos + name.size() + 2));
}

ProbeLayout parse_layout(const std::vector<std::string>& lines) {
    ProbeLayout l;
    for (const std::string& line : lines) {
        if (line.rfind("== layout", 0) == 0) {
            l.ptr = num(field(line, "ptr"));
            l.is_64bit = l.ptr == 8;
        }
        if (line.rfind("layout ", 0) != 0) continue;
        std::istringstream in(line);
        std::string tag, name;
        in >> tag >> name;
        const u32 size = num(field(line, "size"));
        if (name == "DESCRIPTOR_RANGE") {
            l.range_size = size;
        } else if (name == "DESCRIPTOR_RANGE1") {
            l.range1_size = size;
            l.range1_flags = at_offset(line, "flags");
            l.range1_offset = at_offset(line, "offset");
        } else if (name == "ROOT_PARAMETER") {
            l.param_size = size;
            l.param_union = at_offset(line, "union");
            l.param_ranges = at_offset(line, "ranges");
            l.param_vis = at_offset(line, "vis");
        } else if (name == "STATIC_SAMPLER_DESC") {
            l.sampler_size = size;
        } else if (name == "STATIC_SAMPLER_DESC1") {
            l.sampler1_size = size;
            l.sampler1_flags = at_offset(line, "flags");
        } else if (name == "ROOT_SIGNATURE_DESC") {
            l.desc_size = size;
            l.desc_params = at_offset(line, "params");
            l.desc_nsamplers = at_offset(line, "nsamplers");
            l.desc_samplers = at_offset(line, "samplers");
            l.desc_flags = at_offset(line, "flags");
        } else if (name == "VERSIONED_ROOT_SIGNATURE_DESC") {
            l.versioned_size = size;
            l.versioned_desc = at_offset(line, "desc");
        }
    }
    return l;
}

/// A desc from "in" lines, with the NULL-pointer markers the probe prints.
struct InDesc {
    int layout{10};
    d3d12::RootSignatureDesc desc;
};

/// Parse " in desc ..." and the " in ..." lines after it, from @p index.
InDesc parse_in(const std::vector<std::string>& lines, usize& index) {
    InDesc in;
    const std::string& head = lines[index];
    in.layout = std::stoi(field(head, "layout"));
    in.desc.flags = num(field(head, "flags"));
    in.desc.parameters.resize(num(field(head, "params")));
    in.desc.static_samplers.resize(num(field(head, "samplers")));
    ++index;
    usize param = 0;
    usize range = 0;
    usize sampler = 0;
    while (index < lines.size() && lines[index].rfind(" in ", 0) == 0) {
        const std::string& line = lines[index];
        if (line.find(" param ") != std::string::npos &&
            line.find("sampler") == std::string::npos) {
            std::istringstream s(line);
            std::string a, b;
            s >> a >> b >> param;
            d3d12::RootParameter& p = in.desc.parameters[param];
            p.parameter_type = num(field(line, "type"));
            p.shader_visibility = num(field(line, "vis"));
            if (p.parameter_type == 0) {
                p.ranges.resize(num(field(line, "ranges")));
            } else if (p.parameter_type <= 4) {
                p.shader_register = num(field(line, "reg"));
                p.register_space = num(field(line, "space"));
                if (p.parameter_type == 1) {
                    p.num_32bit_values = num(field(line, "num"));
                } else if (!field(line, "flags").empty()) {
                    p.flags = num(field(line, "flags"));
                }
            }
        } else if (line.find(" range ") != std::string::npos) {
            std::istringstream s(line);
            std::string a, b;
            s >> a >> b >> range;
            d3d12::RootDescriptorRange& r =
                in.desc.parameters[param].ranges[range];
            r.range_type = num(field(line, "type"));
            r.num_descriptors = num(field(line, "num"));
            r.base_shader_register = num(field(line, "base"));
            r.register_space = num(field(line, "space"));
            r.offset_in_descriptors_from_table_start =
                num(field(line, "offset"));
            if (!field(line, "flags").empty()) r.flags = num(field(line, "flags"));
        } else if (line.find("sampler ") != std::string::npos) {
            std::istringstream s(line);
            std::string a, b;
            s >> a >> b >> sampler;
            d3d12::StaticSampler& o = in.desc.static_samplers[sampler];
            o.filter = num(field(line, "filter"));
            const std::string addr = field(line, "addr");
            std::sscanf(addr.c_str(), "%u,%u,%u", &o.address_u, &o.address_v,
                        &o.address_w);
            u32 bits = num(field(line, "bias"));
            std::memcpy(&o.mip_lod_bias, &bits, 4);
            o.max_anisotropy = num(field(line, "aniso"));
            o.comparison_func = num(field(line, "cmp"));
            o.border_color = num(field(line, "border"));
            u32 lo = 0, hi = 0;
            std::sscanf(field(line, "lod").c_str(), "0x%X,0x%X", &lo, &hi);
            std::memcpy(&o.min_lod, &lo, 4);
            std::memcpy(&o.max_lod, &hi, 4);
            o.shader_register = num(field(line, "reg"));
            o.register_space = num(field(line, "space"));
            o.shader_visibility = num(field(line, "vis"));
            if (!field(line, "flags").empty()) o.flags = num(field(line, "flags"));
        }
        ++index;
    }
    return in;
}

u32 bits_of(f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, 4);
    return bits;
}

/// The probe's raw RTS0 writer, reimplemented: no validation, the layout the
/// probe checked against every blob Windows produced ("raw=match").
std::vector<u8> raw_blob(const d3d12::RootSignatureDesc& desc, u32 version) {
    std::vector<u8> part;
    const auto put = [&](u32 v) {
        const usize at = part.size();
        part.resize(at + 4);
        std::memcpy(part.data() + at, &v, 4);
    };
    put(version);
    put(static_cast<u32>(desc.parameters.size()));
    put(24);
    put(static_cast<u32>(desc.static_samplers.size()));
    put(0);
    put(desc.flags);
    const usize records = part.size();
    part.resize(records + 12 * desc.parameters.size());
    for (usize i = 0; i < desc.parameters.size(); ++i) {
        const d3d12::RootParameter& p = desc.parameters[i];
        const u32 payload = static_cast<u32>(part.size());
        std::memcpy(part.data() + records + 12 * i, &p.parameter_type, 4);
        std::memcpy(part.data() + records + 12 * i + 4, &p.shader_visibility, 4);
        std::memcpy(part.data() + records + 12 * i + 8, &payload, 4);
        if (p.parameter_type == 0) {
            put(static_cast<u32>(p.ranges.size()));
            put(payload + 8);
            for (const auto& r : p.ranges) {
                put(r.range_type);
                put(r.num_descriptors);
                put(r.base_shader_register);
                put(r.register_space);
                if (version >= 2) put(r.flags);
                put(r.offset_in_descriptors_from_table_start);
            }
        } else if (p.parameter_type == 1) {
            put(p.shader_register);
            put(p.register_space);
            put(p.num_32bit_values);
        } else if (p.parameter_type <= 4) {
            put(p.shader_register);
            put(p.register_space);
            if (version >= 2) put(p.flags);
        }
    }
    const u32 samplers = static_cast<u32>(part.size());
    std::memcpy(part.data() + 16, &samplers, 4);
    for (const auto& s : desc.static_samplers) {
        put(s.filter);
        put(s.address_u);
        put(s.address_v);
        put(s.address_w);
        put(bits_of(s.mip_lod_bias));
        put(s.max_anisotropy);
        put(s.comparison_func);
        put(s.border_color);
        put(bits_of(s.min_lod));
        put(bits_of(s.max_lod));
        put(s.shader_register);
        put(s.register_space);
        put(s.shader_visibility);
        if (version >= 3) put(s.flags);
    }
    std::vector<u8> blob(44 + part.size(), 0);
    const u32 words[] = {0x43425844u, 0, 0, 0, 0, 1,
                         static_cast<u32>(blob.size()), 1, 36, 0x30535452u,
                         static_cast<u32>(part.size())};
    std::memcpy(blob.data(), words, sizeof(words));
    std::memcpy(blob.data() + 44, part.data(), part.size());
    graphics::dxbc_container_checksum(blob.data(),
                                      static_cast<u32>(blob.size()),
                                      blob.data() + 4);
    return blob;
}

/// Byte arrays from the fxc header the probe includes.
std::map<std::string, std::vector<u8>> read_shader_header() {
    std::map<std::string, std::vector<u8>> out;
    std::ifstream file(std::string(VITREO_ORACLE_PROBE_DIR) +
                       "/d3d12_rootsig_shaders.h");
    std::string line;
    std::string current;
    while (std::getline(file, line)) {
        const auto name = line.find("unsigned char ");
        if (name != std::string::npos) {
            const auto start = name + 14;
            current = line.substr(start, line.find('[') - start);
            continue;
        }
        if (current.empty()) continue;
        if (line.find("};") != std::string::npos) {
            current.clear();
            continue;
        }
        std::istringstream in(line);
        std::string token;
        while (std::getline(in, token, ',')) {
            if (token.find_first_of("0123456789") != std::string::npos) {
                out[current].push_back(static_cast<u8>(std::stoul(token)));
            }
        }
    }
    return out;
}

u32 get32(const std::vector<u8>& bytes, usize at) {
    u32 v = 0;
    std::memcpy(&v, bytes.data() + at, 4);
    return v;
}

void put32(std::vector<u8>& bytes, usize at, u32 v) {
    std::memcpy(bytes.data() + at, &v, 4);
}

void rehash(std::vector<u8>& bytes) {
    graphics::dxbc_container_checksum(bytes.data(), get32(bytes, 24),
                                      bytes.data() + 4);
}

// =============================================================================
// The replay fixture: a D3D12Manager, its exports and vtables, guest memory
// =============================================================================

constexpr GuestAddr kData = 0x40000;
constexpr usize kDataSize = 0x40000;
constexpr GuestAddr kStack = kData + kDataSize - 0x1000;
constexpr GuestAddr kOut = kData + 0x100;       // out-parameters
constexpr GuestAddr kIid = kData + 0x200;       // a REFIID
constexpr GuestAddr kArena = kData + 0x1000;    // descs and blob copies
constexpr u64 kSentinel = 0x5A5A5A50ull;

class Replay {
public:
    explicit Replay(bool is_64bit) : m_is_64bit(is_64bit) {
        EXPECT_TRUE(memory.map_region(kData, kDataSize,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.mode = is_64bit ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        manager = std::make_unique<D3D12Manager>(memory, is_64bit, backend);
        exports = create_d3d12_thunks(*manager);
        for (const auto& entry : exports.entries) {
            by_name[entry.name] = &entry;
        }
        // One synthetic thunk address per vtable slot, resolved back to its
        // export when a test calls through the vtable.
        D3D12Manager::MethodThunks thunks{};
        GuestAddr next = 0x70000000;
        const auto fill = [&](const char* interface_name, auto& table) {
            for (usize slot = 0; slot < table.size(); ++slot) {
                table[slot] = next;
                by_thunk[next] = d3d12_method_export_name(interface_name, slot);
                next += 0x10;
            }
        };
        for (usize table = 0; table < d3d12::INTERFACE_TABLE_COUNT; ++table) {
            const auto& row = d3d12::INTERFACE_TABLES[table];
            thunks.tables[table].resize(row.method_count);
            fill(row.export_name, thunks.tables[table]);
        }
        fill("Blob", thunks.blob);
        fill("RootSignatureDeserializer", thunks.root_signature_deserializer);
        fill("VersionedRootSignatureDeserializer",
             thunks.versioned_root_signature_deserializer);
        manager->set_method_thunks(thunks);
    }

    [[nodiscard]] u32 ptr_size() const { return m_is_64bit ? 8u : 4u; }

    [[nodiscard]] u64 read_ptr(GuestAddr at) const {
        return m_is_64bit ? memory.read_u64(at).value() : memory.read_u32(at).value();
    }
    void write_ptr(GuestAddr at, u64 value) {
        EXPECT_TRUE((m_is_64bit ? memory.write_u64(at, value)
                                : memory.write_u32(at, static_cast<u32>(value)))
                        .is_ok());
    }

    /// Call an export (or a vtable thunk) by name with @p args; the result as
    /// the guest sees it (EAX on x86).
    u64 call(const std::string& name, std::vector<u64> args) {
        const auto found = by_name.find(name);
        EXPECT_NE(found, by_name.end()) << name;
        if (found == by_name.end()) return ~0ull;
        if (m_is_64bit) {
            cpu.gpr.rsp = kStack;
            u64* regs[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) {
                    *regs[i] = args[i];
                } else {
                    EXPECT_TRUE(memory.write_u64(kStack + 0x28 + (i - 4) * 8,
                                                 args[i]).is_ok());
                }
            }
        } else {
            cpu.gpr.rsp = kStack;
            // stdcall: every argument on the stack above the return address,
            // and the callee pops exactly stdcall_bytes of them.
            EXPECT_EQ(found->second->stdcall_bytes, args.size() * 4) << name;
            for (usize i = 0; i < args.size(); ++i) {
                EXPECT_TRUE(memory.write_u32(kStack + 4 + i * 4,
                                             static_cast<u32>(args[i])).is_ok());
            }
        }
        cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        found->second->handler(m);
        return m_is_64bit ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
    }

    /// Call slot @p slot of the COM object at @p object through its vtable.
    u64 com(GuestAddr object, usize slot, std::vector<u64> args) {
        const u64 vtable = read_ptr(object);
        EXPECT_NE(vtable, 0u);
        const u64 thunk = read_ptr(vtable + slot * ptr_size());
        const auto name = by_thunk.find(thunk);
        EXPECT_NE(name, by_thunk.end()) << "vtable slot " << slot;
        if (name == by_thunk.end()) return ~0ull;
        args.insert(args.begin(), object);
        return call(name->second, std::move(args));
    }

    GuestAddr put_iid(const std::array<u8, 16>& iid) {
        EXPECT_TRUE(memory.write_bytes(kIid, reinterpret_cast<const Byte*>(iid.data()),
                                       iid.size()).is_ok());
        return kIid;
    }

    /// Copy @p bytes into the arena and return where.
    GuestAddr put_bytes(const std::vector<u8>& bytes) {
        const GuestAddr at = kArena + 0x20000;
        EXPECT_LE(bytes.size(), 0x10000u);
        if (!bytes.empty()) {
            EXPECT_TRUE(memory.write_bytes(at, reinterpret_cast<const Byte*>(bytes.data()),
                                           bytes.size()).is_ok());
        }
        return at;
    }

    std::vector<u8> blob_bytes(GuestAddr blob) {
        const u64 data = com(blob, 3, {});
        const u64 size = com(blob, 4, {});
        std::vector<u8> bytes(size);
        if (size != 0) {
            EXPECT_TRUE(memory.read_bytes(data, reinterpret_cast<Byte*>(bytes.data()),
                                          bytes.size()).is_ok());
        }
        return bytes;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    NullBackend backend;
    std::unique_ptr<D3D12Manager> manager;
    DllExports exports;
    std::unordered_map<std::string, const ExportEntry*> by_name;
    std::unordered_map<u64, std::string> by_thunk;

private:
    bool m_is_64bit;
};

// =============================================================================
// Guest struct writer and dumper, driven by the probe's [layout] offsets
// =============================================================================

/// Lay @p in out at @p at the way the probe's build_10 / build_1x do: every
/// array pointer non-NULL, even for an empty array (they point at the probe's
/// static arrays).
GuestAddr write_in_desc(Replay& r, const ProbeLayout& l, const InDesc& in,
                        GuestAddr at, bool versioned, u32 version) {
    std::vector<u8> block(0x10000, 0);
    const auto p32 = [&](usize off, u32 v) { std::memcpy(block.data() + off, &v, 4); };
    const auto pptr = [&](usize off, u64 v) {
        if (l.ptr == 8) std::memcpy(block.data() + off, &v, 8);
        else p32(off, static_cast<u32>(v));
    };
    const bool v1x = in.layout != 10;
    const usize desc = versioned ? l.versioned_desc : 0;
    usize cursor = 0x100;
    if (versioned) p32(0, version);
    const auto& d = in.desc;
    p32(desc, static_cast<u32>(d.parameters.size()));
    p32(desc + l.desc_nsamplers, static_cast<u32>(d.static_samplers.size()));
    p32(desc + l.desc_flags, d.flags);
    const usize params = cursor;
    cursor += d.parameters.size() * l.param_size;
    pptr(desc + l.desc_params, at + params);
    for (usize i = 0; i < d.parameters.size(); ++i) {
        const auto& p = d.parameters[i];
        const usize base = params + i * l.param_size;
        p32(base, p.parameter_type);
        p32(base + l.param_vis, p.shader_visibility);
        const usize u = base + l.param_union;
        if (p.parameter_type == 0) {
            p32(u, static_cast<u32>(p.ranges.size()));
            pptr(base + l.param_ranges, at + cursor);
            for (const auto& g : p.ranges) {
                p32(cursor, g.range_type);
                p32(cursor + 4, g.num_descriptors);
                p32(cursor + 8, g.base_shader_register);
                p32(cursor + 12, g.register_space);
                if (v1x) {
                    p32(cursor + l.range1_flags, g.flags);
                    p32(cursor + l.range1_offset,
                        g.offset_in_descriptors_from_table_start);
                    cursor += l.range1_size;
                } else {
                    p32(cursor + 16, g.offset_in_descriptors_from_table_start);
                    cursor += l.range_size;
                }
            }
        } else if (p.parameter_type == 1) {
            p32(u, p.shader_register);
            p32(u + 4, p.register_space);
            p32(u + 8, p.num_32bit_values);
        } else if (p.parameter_type <= 4) {
            p32(u, p.shader_register);
            p32(u + 4, p.register_space);
            if (v1x) p32(u + 8, p.flags);
        }
    }
    pptr(desc + l.desc_samplers, at + cursor);
    for (const auto& s : d.static_samplers) {
        const u32 w[14] = {s.filter, s.address_u, s.address_v, s.address_w,
                           bits_of(s.mip_lod_bias), s.max_anisotropy,
                           s.comparison_func, s.border_color, bits_of(s.min_lod),
                           bits_of(s.max_lod), s.shader_register, s.register_space,
                           s.shader_visibility, s.flags};
        const usize size = in.layout == 12 ? l.sampler1_size : l.sampler_size;
        std::memcpy(block.data() + cursor, w, size);
        cursor += size;
    }
    EXPECT_TRUE(r.memory.write_bytes(at, reinterpret_cast<const Byte*>(block.data()),
                                     cursor).is_ok());
    return at;
}

/// dump_desc_10 / dump_desc_1x / dump_versioned, reading guest memory.
class Dumper {
public:
    Dumper(Replay& r, const ProbeLayout& l) : m_r(r), m_l(l) {}

    void desc(std::vector<std::string>& out, const std::string& ind,
              GuestAddr at, int layout) {
        auto& mem = m_r.memory;
        const u32 flags = mem.read_u32(at + m_l.desc_flags).value();
        const u32 np = mem.read_u32(at).value();
        const u64 pp = m_r.read_ptr(at + m_l.desc_params);
        const u32 ns = mem.read_u32(at + m_l.desc_nsamplers).value();
        const u64 ps = m_r.read_ptr(at + m_l.desc_samplers);
        out.push_back(fmt("%sdesc layout=%d flags=0x%X params=%u%s samplers=%u%s",
                          ind.c_str(), layout, flags, np,
                          pp ? "" : " pParameters=null", ns,
                          ps ? "" : " pStaticSamplers=null"));
        const bool v1x = layout != 10;
        if (pp) {
            for (u32 i = 0; i < np; ++i) {
                const GuestAddr p = pp + static_cast<u64>(i) * m_l.param_size;
                const u32 type = mem.read_u32(p).value();
                const u32 vis = mem.read_u32(p + m_l.param_vis).value();
                std::string line = fmt("%s param %u type=%u vis=%u", ind.c_str(), i,
                                       type, vis);
                const GuestAddr u = p + m_l.param_union;
                if (type == 0) {
                    const u32 n = mem.read_u32(u).value();
                    const u64 rp = m_r.read_ptr(p + m_l.param_ranges);
                    out.push_back(line + fmt(" ranges=%u%s", n,
                                             rp ? "" : " pDescriptorRanges=null"));
                    for (u32 k = 0; rp && k < n; ++k) {
                        const GuestAddr g =
                            rp + static_cast<u64>(k) *
                                     (v1x ? m_l.range1_size : m_l.range_size);
                        std::string rl = fmt(
                            "%s  range %u type=%u num=%u base=%u space=%u offset=%u",
                            ind.c_str(), k, mem.read_u32(g).value(),
                            mem.read_u32(g + 4).value(), mem.read_u32(g + 8).value(),
                            mem.read_u32(g + 12).value(),
                            mem.read_u32(g + (v1x ? m_l.range1_offset : 16)).value());
                        if (v1x) {
                            rl += fmt(" flags=0x%X",
                                      mem.read_u32(g + m_l.range1_flags).value());
                        }
                        out.push_back(rl);
                    }
                } else if (type == 1) {
                    out.push_back(line + fmt(" reg=%u space=%u num=%u",
                                             mem.read_u32(u).value(),
                                             mem.read_u32(u + 4).value(),
                                             mem.read_u32(u + 8).value()));
                } else if (type <= 4) {
                    line += fmt(" reg=%u space=%u", mem.read_u32(u).value(),
                                mem.read_u32(u + 4).value());
                    if (v1x) line += fmt(" flags=0x%X", mem.read_u32(u + 8).value());
                    out.push_back(line);
                } else {
                    out.push_back(line);
                }
            }
        }
        if (ps) {
            const u32 size = layout == 12 ? m_l.sampler1_size : m_l.sampler_size;
            for (u32 i = 0; i < ns; ++i) {
                u32 w[14] = {};
                EXPECT_TRUE(mem.read_bytes(ps + static_cast<u64>(i) * size,
                                           reinterpret_cast<Byte*>(w), size).is_ok());
                std::string line = fmt(
                    "%ssampler %u filter=0x%X addr=%u,%u,%u bias=0x%08X aniso=%u "
                    "cmp=%u border=%u lod=0x%08X,0x%08X reg=%u space=%u vis=%u",
                    ind.c_str(), i, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                    w[8], w[9], w[10], w[11], w[12]);
                if (layout == 12) line += fmt(" flags=0x%X", w[13]);
                out.push_back(line);
            }
        }
    }

    void versioned(std::vector<std::string>& out, const std::string& ind,
                   GuestAddr at) {
        const u32 version = m_r.memory.read_u32(at).value();
        out.push_back(fmt("%sversion=%u", ind.c_str(), version));
        if (version >= 1 && version <= 3) {
            desc(out, ind, at + m_l.versioned_desc,
                 version == 1 ? 10 : version == 2 ? 11 : 12);
        }
    }

private:
    Replay& m_r;
    const ProbeLayout& m_l;
};

const char* state_of(u64 value, bool passed_null) {
    if (passed_null) return "-";
    if (value == 0) return "null";
    if (value == kSentinel) return "untouched";
    return "set";
}

const char* kind_of(u64 value) {
    if (value == 0) return "null";
    if (value == kSentinel) return "untouched";
    return "other";
}

void print_bytes(std::vector<std::string>& out, const char* tag,
                 const std::vector<u8>& bytes) {
    out.push_back(fmt("%s %u", tag, static_cast<unsigned>(bytes.size())));
    for (usize i = 0; i < bytes.size(); i += 32) {
        std::string line = " ";
        for (usize k = i; k < std::min(bytes.size(), i + 32); ++k) {
            line += fmt("%02X", bytes[k]);
        }
        out.push_back(line);
    }
}

void print_text(std::vector<std::string>& out, const char* tag,
                const std::vector<u8>& bytes) {
    std::string line = fmt("%s %u \"", tag, static_cast<unsigned>(bytes.size()));
    for (u8 c : bytes) {
        if (c == '\n') line += "\\n";
        else if (c == '\r') line += "\\r";
        else if (c == '\t') line += "\\t";
        else if (c == '"') line += "\\\"";
        else if (c == '\\') line += "\\\\";
        else if (c < 0x20 || c >= 0x7F) line += fmt("\\x%02X", c);
        else line += static_cast<char>(c);
    }
    out.push_back(line + "\"");
}

/// Compare generated lines with the expected ones and report the first
/// difference with its neighbourhood.
void expect_same(const std::vector<std::string>& expected,
                 const std::vector<std::string>& actual, const char* what) {
    const usize n = std::min(expected.size(), actual.size());
    for (usize i = 0; i < n; ++i) {
        if (expected[i] != actual[i]) {
            std::string context;
            for (usize k = i >= 6 ? i - 6 : 0; k < i; ++k) {
                context += "   " + expected[k] + "\n";
            }
            ADD_FAILURE() << what << ": line " << i << " differs\n"
                          << context << "   Windows: " << expected[i]
                          << "\n   Vitreo:  " << actual[i];
            return;
        }
    }
    EXPECT_EQ(expected.size(), actual.size()) << what << ": line count";
}

// =============================================================================
// The whole probe, replayed
// =============================================================================

struct Stored {
    std::string name;
    std::vector<u8> bytes;
};

class ProbeReplay {
public:
    explicit ProbeReplay(bool is_64bit)
        : m_r(is_64bit),
          m_lines(read_lines(is_64bit ? "d3d12_rootsig_probe64.expected"
                                      : "d3d12_rootsig_probe32.expected")),
          m_l(parse_layout(m_lines)),
          m_dump(m_r, m_l) {}

    [[nodiscard]] bool loaded() const { return !m_lines.empty(); }
    [[nodiscard]] const ProbeLayout& layout() const { return m_l; }
    [[nodiscard]] const std::vector<std::string>& lines() const { return m_lines; }
    Replay& replay() { return m_r; }

    /// [ser]: every case through its export.
    void ser() {
        const std::vector<std::string> expected = filtered(section(m_lines, "ser"));
        std::vector<std::string> out;
        for (usize i = 0; i < expected.size();) {
            const std::string header = expected[i];
            if (header.rfind("case ", 0) != 0) {
                ADD_FAILURE() << "unexpected line " << header;
                return;
            }
            ++i;
            usize at = i;
            const InDesc in = parse_in(expected, at);
            i = at;
            while (i < expected.size() && expected[i].rfind("case ", 0) != 0) ++i;
            serialize_case(out, header, in);
        }
        expect_same(expected, out, "[ser]");
    }

    /// [null]
    void null() {
        std::vector<std::string> out;
        Replay& r = m_r;
        const auto reset_out = [&] {
            r.write_ptr(kOut, kSentinel);
            r.write_ptr(kOut + 8, kSentinel);
        };
        reset_out();
        u32 hr = static_cast<u32>(r.call("D3D12SerializeRootSignature",
                                          {0, 1, kOut, kOut + 8}));
        out.push_back(fmt("S10 desc=null hr=0x%08X blob=%s err=%s", hr,
                          state_of(r.read_ptr(kOut), false),
                          state_of(r.read_ptr(kOut + 8), false)));
        reset_out();
        hr = static_cast<u32>(
            r.call("D3D12SerializeVersionedRootSignature", {0, kOut, kOut + 8}));
        out.push_back(fmt("SV desc=null hr=0x%08X blob=%s err=%s", hr,
                          state_of(r.read_ptr(kOut), false),
                          state_of(r.read_ptr(kOut + 8), false)));
        for (u64 size : {0ull, 64ull}) {
            for (int versioned = 0; versioned < 2; ++versioned) {
                reset_out();
                const char* name = versioned
                                       ? "D3D12CreateVersionedRootSignatureDeserializer"
                                       : "D3D12CreateRootSignatureDeserializer";
                const GuestAddr iid = r.put_iid(
                    versioned ? d3d12::IID_ID3D12VERSIONEDROOTSIGNATUREDESERIALIZER
                              : d3d12::IID_ID3D12ROOTSIGNATUREDESERIALIZER);
                hr = static_cast<u32>(r.call(name, {0, size, iid, kOut}));
                out.push_back(fmt("%s data=null size=%u hr=0x%08X out=%s", name,
                                  static_cast<unsigned>(size), hr,
                                  kind_of(r.read_ptr(kOut))));
            }
        }
        expect_same(section(m_lines, "null"), out, "[null]");
    }

    /// [blob]: ID3DBlob identity on a blob and an error blob.
    void blob() {
        std::vector<std::string> out;
        Replay& r = m_r;
        InDesc empty;
        const GuestAddr desc = write_in_desc(r, m_l, empty, kArena, false, 1);
        r.write_ptr(kOut, 0);
        r.write_ptr(kOut + 8, 0);
        u32 hr = static_cast<u32>(
            r.call("D3D12SerializeRootSignature", {desc, 1, kOut, kOut + 8}));
        out.push_back(fmt("serialize empty hr=0x%08X", hr));
        identity(out, r.read_ptr(kOut));
        InDesc bad;
        bad.desc.parameters.resize(1);
        bad.desc.parameters[0].shader_visibility = 9;
        write_in_desc(r, m_l, bad, kArena, false, 1);
        r.write_ptr(kOut, 0);
        r.write_ptr(kOut + 8, 0);
        hr = static_cast<u32>(
            r.call("D3D12SerializeRootSignature", {desc, 1, kOut, kOut + 8}));
        out.push_back(fmt("serialize bad hr=0x%08X blob=%s", hr,
                          r.read_ptr(kOut) ? "set" : "null"));
        identity(out, r.read_ptr(kOut + 8));
        expect_same(section(m_lines, "blob"), out, "[blob]");
    }

    /// [deser]: both deserializers over every distinct blob [ser] made.
    void deser() {
        std::vector<std::string> out;
        for (const Stored& s : m_blobs) deserialize(out, s.name, s.bytes);
        expect_same(section(m_lines, "deser"), out, "[deser]");
    }

    /// [scan]: through the seam, which the exports wrap.
    void scan() {
        std::vector<std::string> out;
        const auto scan_field = [&](const char* label, u32 filter, u32 address,
                                    int which, u32 lo, u32 hi) {
            std::string line = fmt("scan %s filter=0x%X addr=%u ok:", label, filter,
                                   address);
            for (u32 v = lo; v <= hi; ++v) {
                d3d12::RootSignatureDesc d;
                d3d12::StaticSampler s;
                s.filter = filter;
                s.address_u = s.address_v = s.address_w = address;
                s.max_anisotropy = 16;
                s.comparison_func = 8;
                s.max_lod = 3.402823466e+38f;
                switch (which) {
                case 0: s.filter = v; break;
                case 1: s.address_u = v; break;
                case 2: s.comparison_func = v; break;
                case 3: s.border_color = v; break;
                case 4: s.max_anisotropy = v; break;
                }
                d.static_samplers.push_back(s);
                std::vector<u8> blob;
                std::string message;
                if (d3d12::serialize_root_signature(d, 1, blob, message) ==
                    d3d12::S_OK) {
                    line += fmt(" %X", v);
                }
            }
            out.push_back(line);
        };
        scan_field("filter", 0, 1, 0, 0, 0x3FF);
        scan_field("address", 0x15, 1, 1, 0, 16);
        scan_field("cmp", 0x95, 1, 2, 0, 16);
        scan_field("cmp", 0x15, 1, 2, 0, 16);
        scan_field("border", 0x15, 4, 3, 0, 16);
        scan_field("border", 0x15, 1, 3, 0, 16);
        scan_field("aniso", 0x55, 1, 4, 0, 32);
        scan_field("aniso", 0x15, 1, 4, 0, 32);
        const char* labels[] = {"rootsig_flags", "range_flags_srv",
                                "range_flags_sampler", "root_descriptor_flags",
                                "sampler_flags"};
        for (int what = 0; what < 5; ++what) {
            std::string line = fmt("scan %s ok:", labels[what]);
            for (u32 bit = 0; bit < 32; ++bit) {
                const u32 f = 1u << bit;
                d3d12::RootSignatureDesc d;
                u32 version = 2;
                d3d12::RootParameter p;
                d3d12::RootDescriptorRange range;
                range.num_descriptors = 1;
                range.offset_in_descriptors_from_table_start = 0xFFFFFFFFu;
                range.flags = f;
                switch (what) {
                case 0: d.flags = f; version = 1; break;
                case 1: p.ranges.push_back(range); d.parameters.push_back(p); break;
                case 2:
                    range.range_type = 3;
                    p.ranges.push_back(range);
                    d.parameters.push_back(p);
                    break;
                case 3:
                    p.parameter_type = 2;
                    p.flags = f;
                    d.parameters.push_back(p);
                    break;
                case 4: {
                    d3d12::StaticSampler s;
                    s.address_u = s.address_v = s.address_w = 3;
                    s.max_anisotropy = 16;
                    s.comparison_func = 8;
                    s.flags = f;
                    d.static_samplers.push_back(s);
                    version = 3;
                    break;
                }
                }
                std::vector<u8> blob;
                std::string message;
                if (d3d12::serialize_root_signature(d, version, blob, message) ==
                    d3d12::S_OK) {
                    line += fmt(" %X", f);
                }
            }
            out.push_back(line);
        }
        expect_same(section(m_lines, "scan"), out, "[scan]");
    }

    /// [ident]: the deserializers' COM identity and argument order.
    void ident() {
        std::vector<std::string> out;
        Replay& r = m_r;
        ASSERT_FALSE(m_blobs.empty());
        const GuestAddr data = r.put_bytes(m_blobs[0].bytes);
        const u64 size = m_blobs[0].bytes.size();
        struct Iid { const char* name; std::array<u8, 16> bytes; };
        const Iid iids[] = {
            {"IUnknown", d3d12::IID_IUNKNOWN},
            {"Deserializer", d3d12::IID_ID3D12ROOTSIGNATUREDESERIALIZER},
            {"VersionedDeserializer",
             d3d12::IID_ID3D12VERSIONEDROOTSIGNATUREDESERIALIZER},
            {"ID3D10Blob", d3d12::IID_ID3D10BLOB},
            {"Bogus", {0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x9a, 0xbc,
                       0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}},
        };
        const GuestAddr second_iid = kIid + 0x40;
        for (int creator = 0; creator < 2; ++creator) {
            const char* name = creator == 0
                                   ? "D3D12CreateRootSignatureDeserializer"
                                   : "D3D12CreateVersionedRootSignatureDeserializer";
            for (const Iid& iid : iids) {
                r.write_ptr(kOut, kSentinel);
                const u32 hr = static_cast<u32>(
                    r.call(name, {data, size, r.put_iid(iid.bytes), kOut}));
                const u64 object = r.read_ptr(kOut);
                out.push_back(fmt("%s riid=%s hr=0x%08X out=%s", name, iid.name, hr,
                                  kind_of(object)));
                if (hr != 0 || object == 0 || object == kSentinel) continue;
                for (const Iid& q : iids) {
                    ASSERT_TRUE(r.memory.write_bytes(
                        second_iid, reinterpret_cast<const Byte*>(q.bytes.data()),
                        16).is_ok());
                    r.write_ptr(kOut + 8, kSentinel);
                    const u32 qhr =
                        static_cast<u32>(r.com(object, 0, {second_iid, kOut + 8}));
                    const u64 got = r.read_ptr(kOut + 8);
                    out.push_back(fmt(" qi %s hr=0x%08X out=%s", q.name, qhr,
                                      got == object ? "self" : kind_of(got)));
                    if (qhr == 0 && got != 0 && got != kSentinel) r.com(got, 2, {});
                }
                out.push_back(fmt(" addref=%u", static_cast<u32>(r.com(object, 1, {}))));
                out.push_back(fmt(" release=%u", static_cast<u32>(r.com(object, 2, {}))));
                out.push_back(fmt(" release=%u", static_cast<u32>(r.com(object, 2, {}))));
            }
        }
        const u32 a = static_cast<u32>(r.call(
            "D3D12CreateRootSignatureDeserializer",
            {data, size, r.put_iid(iids[1].bytes), 0}));
        out.push_back(fmt("D3D12CreateRootSignatureDeserializer ppv=null hr=0x%08X", a));
        const u32 b = static_cast<u32>(r.call(
            "D3D12CreateVersionedRootSignatureDeserializer",
            {data, size, r.put_iid(iids[2].bytes), 0}));
        out.push_back(
            fmt("D3D12CreateVersionedRootSignatureDeserializer ppv=null hr=0x%08X", b));
        std::vector<u8> junk(64, 0);
        junk[0] = 1;
        junk[1] = 2;
        junk[2] = 3;
        for (int creator = 0; creator < 2; ++creator) {
            const char* name = creator == 0
                                   ? "D3D12CreateRootSignatureDeserializer"
                                   : "D3D12CreateVersionedRootSignatureDeserializer";
            const std::array<u8, 16>& good = creator == 0 ? iids[1].bytes
                                                          : iids[2].bytes;
            const GuestAddr junk_at = kArena + 0x30000;
            ASSERT_TRUE(r.memory.write_bytes(junk_at,
                                             reinterpret_cast<const Byte*>(junk.data()),
                                             junk.size()).is_ok());
            r.write_ptr(kOut, kSentinel);
            u32 hr = static_cast<u32>(
                r.call(name, {junk_at, 64, r.put_iid(iids[4].bytes), kOut}));
            out.push_back(fmt("%s data=junk riid=Bogus hr=0x%08X out=%s", name, hr,
                              kind_of(r.read_ptr(kOut))));
            hr = static_cast<u32>(r.call(name, {junk_at, 64, r.put_iid(good), 0}));
            out.push_back(fmt("%s data=junk ppv=null hr=0x%08X", name, hr));
            hr = static_cast<u32>(
                r.call(name, {data, size, r.put_iid(iids[4].bytes), 0}));
            out.push_back(fmt("%s riid=Bogus ppv=null hr=0x%08X", name, hr));
            r.write_ptr(kOut, 0);
            r.call(name, {data, size, r.put_iid(good), kOut});
            const u64 object = r.read_ptr(kOut);
            if (object != 0) {
                out.push_back(fmt("%s qi(own) ppv=null hr=0x%08X", name,
                                  static_cast<u32>(r.com(object, 0, {r.put_iid(good), 0}))));
                out.push_back(fmt("%s qi(Bogus) ppv=null hr=0x%08X", name,
                                  static_cast<u32>(r.com(
                                      object, 0, {r.put_iid(iids[4].bytes), 0}))));
                out.push_back(fmt("%s release=%u", name,
                                  static_cast<u32>(r.com(object, 2, {}))));
            }
            r.write_ptr(kOut, kSentinel);
            hr = static_cast<u32>(r.call(name, {0, 0, r.put_iid(iids[4].bytes), kOut}));
            out.push_back(fmt("%s data=null riid=Bogus hr=0x%08X out=%s", name, hr,
                              kind_of(r.read_ptr(kOut))));
        }
        expect_same(section(m_lines, "ident"), out, "[ident]");
    }

    /// [parse]: the mutated containers, bare parts and fxc output.
    void parse() {
        std::vector<std::string> out;
        for_each_parse_input([&](const std::string& header, const std::string& name,
                                 const std::vector<u8>& bytes, usize size) {
            if (!header.empty()) out.push_back(header);
            deserialize(out, name, bytes, size);
        });
        expect_same(section(m_lines, "parse"), out, "[parse]");
    }

    /// Every [parse] input: (the "mutation" line or "", the name, the bytes,
    /// the size handed to the creators).
    void for_each_parse_input(
        const std::function<void(const std::string&, const std::string&,
                                 const std::vector<u8>&, usize)>& visit) {
        static const std::map<std::string, std::string> kFxc = {
            {"fxc_rootsig_1_0", "g_rs10"}, {"fxc_rootsig_1_1", "g_rs11"},
            {"fxc_vs_5_1_with_rootsig", "g_vs51rs"},
            {"fxc_vs_5_0_without_rootsig", "g_vs50"}};
        const auto shaders = read_shader_header();
        const std::vector<std::string> lines = section(m_lines, "parse");
        for (usize i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            if (line.rfind("mutation ", 0) == 0) {
                std::istringstream in(line);
                std::string tag, name, of, src;
                in >> tag >> name >> of >> src;
                usize size = 0;
                if (i + 1 < lines.size() && lines[i + 1].rfind("deser ", 0) == 0) {
                    size = num(field(lines[i + 1], "size"));
                }
                std::vector<u8> bytes = mutation(line, name, src, size);
                visit(line, name, bytes, size);
            } else if (line.rfind("deser fxc_", 0) == 0) {
                std::istringstream in(line);
                std::string tag, name;
                in >> tag >> name;
                const auto& bytes = shaders.at(kFxc.at(name));
                visit("", name, bytes, bytes.size());
            }
        }
    }

    [[nodiscard]] const std::vector<Stored>& blobs() const { return m_blobs; }

    /// Every [ser] case as (header line, desc) pairs.
    std::vector<std::pair<std::string, InDesc>> ser_cases() const {
        std::vector<std::pair<std::string, InDesc>> cases;
        const std::vector<std::string> lines = section(m_lines, "ser");
        for (usize i = 0; i < lines.size(); ++i) {
            if (lines[i].rfind("case ", 0) != 0) continue;
            usize at = i + 1;
            cases.emplace_back(lines[i], parse_in(lines, at));
        }
        return cases;
    }

private:
    /// The probe's own consistency line; not a Windows result.
    static std::vector<std::string> filtered(std::vector<std::string> lines) {
        std::vector<std::string> out;
        for (auto& line : lines) {
            if (line.rfind(" raw=", 0) != 0) out.push_back(std::move(line));
        }
        return out;
    }

    void serialize_case(std::vector<std::string>& out, const std::string& header,
                        const InDesc& in) {
        Replay& r = m_r;
        std::istringstream s(header);
        std::string tag, name;
        s >> tag >> name;
        const bool s10 = field(header, "api") == "S10";
        const u32 version = num(field(header, "version"));
        const bool null_blob = has_token(header, "ppBlob=null");
        const bool null_error = has_token(header, "ppErrorBlob=null");
        out.push_back(header);
        const GuestAddr desc = write_in_desc(r, m_l, in, kArena, !s10, version);
        Dumper(r, m_l).desc(out, " in ",
                            s10 ? desc : desc + m_l.versioned_desc, in.layout);
        r.write_ptr(kOut, kSentinel);
        r.write_ptr(kOut + 8, kSentinel);
        const u64 blob_out = null_blob ? 0 : kOut;
        const u64 error_out = null_error ? 0 : kOut + 8;
        const u32 hr = static_cast<u32>(
            s10 ? r.call("D3D12SerializeRootSignature",
                         {desc, version, blob_out, error_out})
                : r.call("D3D12SerializeVersionedRootSignature",
                         {desc, blob_out, error_out}));
        const u64 blob = r.read_ptr(kOut);
        const u64 error = r.read_ptr(kOut + 8);
        out.push_back(fmt(" hr=0x%08X blob=%s err=%s", hr, state_of(blob, null_blob),
                          state_of(error, null_error)));
        if (!null_blob && blob != 0 && blob != kSentinel) {
            std::vector<u8> bytes = r.blob_bytes(blob);
            const Stored* dup = nullptr;
            for (const Stored& stored : m_blobs) {
                if (stored.bytes == bytes) {
                    dup = &stored;
                    break;
                }
            }
            if (dup != nullptr) {
                out.push_back(" blob same-as " + dup->name);
            } else {
                print_bytes(out, " blob", bytes);
                m_blobs.push_back({fmt("%s/%s%u", name.c_str(), s10 ? "S10v" : "SVv",
                                       version),
                                   std::move(bytes)});
            }
            EXPECT_EQ(r.com(blob, 2, {}), 0u) << name;
        }
        if (!null_error && error != 0 && error != kSentinel) {
            print_text(out, " err", r.blob_bytes(error));
            EXPECT_EQ(r.com(error, 2, {}), 0u) << name;
        }
    }

    void identity(std::vector<std::string>& out, GuestAddr object) {
        Replay& r = m_r;
        if (object == 0) return;
        const std::pair<const char*, std::array<u8, 16>> iids[] = {
            {"IUnknown", d3d12::IID_IUNKNOWN},
            {"ID3D10Blob", d3d12::IID_ID3D10BLOB},
            {"Bogus", {0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x9a, 0xbc,
                       0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}}};
        for (const auto& [name, iid] : iids) {
            r.write_ptr(kOut + 16, kSentinel);
            const u32 hr =
                static_cast<u32>(r.com(object, 0, {r.put_iid(iid), kOut + 16}));
            const u64 got = r.read_ptr(kOut + 16);
            out.push_back(fmt(" qi %s hr=0x%08X out=%s", name, hr,
                              got == 0 ? "null"
                              : got == kSentinel ? "untouched"
                              : got == object   ? "self"
                                                : "other"));
            if (hr == 0) r.com(object, 2, {});
        }
        out.push_back(fmt(" addref=%u", static_cast<u32>(r.com(object, 1, {}))));
        out.push_back(fmt(" release=%u", static_cast<u32>(r.com(object, 2, {}))));
        out.push_back(fmt(" release=%u", static_cast<u32>(r.com(object, 2, {}))));
    }

    void deserialize(std::vector<std::string>& out, const std::string& name,
                     const std::vector<u8>& source, usize size = ~usize{0}) {
        Replay& r = m_r;
        if (size == ~usize{0}) size = source.size();
        std::vector<u8> bytes = source;
        bytes.resize(std::max(bytes.size(), size), 0);
        const GuestAddr data = r.put_bytes(bytes);
        out.push_back(fmt("deser %s size=%u", name.c_str(), static_cast<unsigned>(size)));
        r.write_ptr(kOut, kSentinel);
        u32 hr = static_cast<u32>(r.call(
            "D3D12CreateRootSignatureDeserializer",
            {data, size, r.put_iid(d3d12::IID_ID3D12ROOTSIGNATUREDESERIALIZER),
             kOut}));
        u64 object = r.read_ptr(kOut);
        out.push_back(fmt(" D3D12CreateRootSignatureDeserializer hr=0x%08X out=%s", hr,
                          kind_of(object)));
        if (hr == 0 && object != 0 && object != kSentinel) {
            const u64 a = r.com(object, 3, {});
            const u64 b = r.com(object, 3, {});
            out.push_back(fmt(" GetRootSignatureDesc %s stable=%d", a ? "set" : "null",
                              a == b));
            if (a) m_dump.desc(out, "  ", a, 10);
            out.push_back(fmt(" release=%u", static_cast<u32>(r.com(object, 2, {}))));
        }
        r.write_ptr(kOut, kSentinel);
        hr = static_cast<u32>(r.call(
            "D3D12CreateVersionedRootSignatureDeserializer",
            {data, size,
             r.put_iid(d3d12::IID_ID3D12VERSIONEDROOTSIGNATUREDESERIALIZER),
             kOut}));
        object = r.read_ptr(kOut);
        out.push_back(fmt(" D3D12CreateVersionedRootSignatureDeserializer hr=0x%08X "
                          "out=%s",
                          hr, kind_of(object)));
        if (hr != 0 || object == 0 || object == kSentinel) return;
        const u64 u = r.com(object, 4, {});
        out.push_back(fmt(" GetUnconvertedRootSignatureDesc %s", u ? "set" : "null"));
        if (u) m_dump.versioned(out, "  ", u);
        for (u32 version = 0; version <= 4; ++version) {
            r.write_ptr(kOut + 8, kSentinel);
            const u32 h1 = static_cast<u32>(r.com(object, 3, {version, kOut + 8}));
            const u64 c = r.read_ptr(kOut + 8);
            r.write_ptr(kOut + 8, kSentinel);
            const u32 h2 = static_cast<u32>(r.com(object, 3, {version, kOut + 8}));
            const u64 c2 = r.read_ptr(kOut + 8);
            out.push_back(fmt(" AtVersion %u hr=0x%08X out=%s unconverted=%d stable=%d "
                              "hr2=0x%08X",
                              version, h1, kind_of(c), c == u, c == c2, h2));
            if (h1 == 0 && c != 0 && c != kSentinel && c != u) {
                m_dump.versioned(out, "  ", c);
            }
        }
        out.push_back(fmt(" unconverted_stable=%d", u == r.com(object, 4, {})));
        out.push_back(fmt(" release=%u", static_cast<u32>(r.com(object, 2, {}))));
    }

    const Stored* find_blob(const std::string& name) const {
        for (const Stored& s : m_blobs) {
            if (s.name == name) return &s;
        }
        ADD_FAILURE() << "no blob " << name;
        return nullptr;
    }

    /// Rebuild one [parse] input from its "mutation" line (the probe's
    /// mutate() and its hand-built containers).
    std::vector<u8> mutation(const std::string& line, const std::string& name,
                             const std::string& src_name, usize size) {
        const Stored* src = find_blob(src_name);
        if (src == nullptr) return {};
        const std::vector<u8>& ref = src->bytes;
        if (name == "bare_rts0_part") {
            return {ref.begin() + 44, ref.begin() + 44 + get32(ref, 40)};
        }
        if (name == "bare_rts0_with_header") {
            return {ref.begin() + 36, ref.begin() + 44 + get32(ref, 40)};
        }
        if (name == "two_parts_rts0_second") {
            const u32 part = get32(ref, 40) + 8;
            std::vector<u8> w(4096, 0);
            std::memcpy(w.data(), ref.data(), 32);
            put32(w, 28, 2);
            put32(w, 32, 40);
            put32(w, 36, 52);
            std::memcpy(w.data() + 40, "XXXX\x04\x00\x00\x00\x11\x22\x33\x44", 12);
            std::memcpy(w.data() + 52, ref.data() + 36, part);
            put32(w, 24, 52 + part);
            rehash(w);
            return w;
        }
        if (name == "two_rts0_parts") {
            std::istringstream in(line);
            std::string t, n, of, a, plus, b;
            in >> t >> n >> of >> a >> plus >> b;
            const Stored* other = find_blob(b);
            if (other == nullptr) return {};
            const u32 part = get32(ref, 40) + 8;
            const u32 part2 = get32(other->bytes, 40) + 8;
            std::vector<u8> w(4096, 0);
            std::memcpy(w.data(), ref.data(), 32);
            put32(w, 28, 2);
            put32(w, 32, 40);
            put32(w, 36, 40 + part);
            std::memcpy(w.data() + 40, ref.data() + 36, part);
            std::memcpy(w.data() + 40 + part, other->bytes.data() + 36, part2);
            put32(w, 24, 40 + part + part2);
            rehash(w);
            return w;
        }
        std::vector<u8> w(4096, 0);
        std::memcpy(w.data(), ref.data(), ref.size());
        const int off = std::stoi(field(line, "off"));
        if (off >= 0) put32(w, static_cast<usize>(off), num(field(line, "value")));
        if (field(line, "rehash") == "1") rehash(w);
        (void)size;
        return w;
    }

    Replay m_r;
    std::vector<std::string> m_lines;
    ProbeLayout m_l;
    Dumper m_dump;
    std::vector<Stored> m_blobs;
};

class D3D12RootSignatureProbe : public ::testing::TestWithParam<bool> {};

// Each section in its own test would re-run [ser] for the blobs the later
// sections need, so one test per bitness replays the probe in its order.
TEST_P(D3D12RootSignatureProbe, ReplaysEverySectionAgainstWindows) {
    ProbeReplay probe(GetParam());
    ASSERT_TRUE(probe.loaded()) << "tests/oracle_data/d3d12_rootsig_probe*.expected";
    ASSERT_EQ(probe.layout().ptr, GetParam() ? 8u : 4u);
    probe.ser();
    probe.null();
    probe.blob();
    probe.deser();
    probe.scan();
    probe.ident();
    probe.parse();
}

INSTANTIATE_TEST_SUITE_P(Bitness, D3D12RootSignatureProbe, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("x64")
                                               : std::string("x86");
                         });

// CreateRootSignature's acceptance, which differs from the deserializers'
// (container hash unchecked, E_FAIL vs E_INVALIDARG, the 64-DWORD budget,
// reserved spaces, wider flag masks), replayed against the seam D12CORE's
// CreateRootSignature calls.
TEST(D3D12RootSignatureDevice, ReplaysCreateRootSignatureAgainstWindows) {
    const std::vector<std::string> device =
        read_lines("d3d12_rootsig_probe_device64.expected");
    ASSERT_FALSE(device.empty());
    ProbeReplay probe(true);
    ASSERT_TRUE(probe.loaded());
    probe.ser();  // the blobs the device run fed to CreateRootSignature
    const auto cases = probe.ser_cases();
    const auto create = [](const std::vector<u8>& bytes, usize size) {
        d3d12::RootSignatureDesc desc;
        std::vector<u8> padded = bytes;
        padded.resize(std::max(padded.size(), size), 0);
        return d3d12::parse_root_signature_blob_for_device(padded.data(), size,
                                                           desc);
    };
    const auto result = [](const char* tag, u32 hr) {
        return fmt("%s hr=0x%08X out=%s", tag, hr, hr == 0 ? "other" : "null");
    };

    // [ser]: " create-raw" after each versioned case at its own layout.
    std::vector<std::string> expected;
    std::vector<std::string> actual;
    usize next_case = 0;
    for (const std::string& line : section(device, "ser")) {
        if (line.rfind("case ", 0) == 0) {
            ASSERT_LT(next_case, cases.size());
            ASSERT_EQ(line, cases[next_case].first);
            ++next_case;
            continue;
        }
        if (line.rfind(" create-raw", 0) != 0) continue;
        const InDesc& in = cases[next_case - 1].second;
        const u32 version = in.layout == 10 ? 1 : in.layout == 11 ? 2 : 3;
        expected.push_back(cases[next_case - 1].first + line);
        const std::vector<u8> raw = raw_blob(in.desc, version);
        actual.push_back(cases[next_case - 1].first +
                         result(" create-raw", create(raw, raw.size())));
    }
    expect_same(expected, actual, "device [ser]");

    // [deser] and [parse]: "create NAME size=S hr=..".
    expected.clear();
    actual.clear();
    for (const std::string& line : section(device, "deser")) expected.push_back(line);
    for (const Stored& s : probe.blobs()) {
        actual.push_back(fmt("create %s size=%u", s.name.c_str(),
                             static_cast<unsigned>(s.bytes.size())) +
                         result("", create(s.bytes, s.bytes.size())));
    }
    expect_same(expected, actual, "device [deser]");

    expected.clear();
    actual.clear();
    for (const std::string& line : section(device, "parse")) {
        if (line.rfind("create ", 0) == 0) expected.push_back(line);
    }
    probe.for_each_parse_input([&](const std::string&, const std::string& name,
                                   const std::vector<u8>& bytes, usize size) {
        actual.push_back(fmt("create %s size=%u", name.c_str(),
                             static_cast<unsigned>(size)) +
                         result("", create(bytes, size)));
    });
    expect_same(expected, actual, "device [parse]");

    // [devscan]
    expected = section(device, "devscan");
    actual.clear();
    const auto raw_hr = [&](const d3d12::RootSignatureDesc& d, u32 version) {
        const std::vector<u8> raw = raw_blob(d, version);
        return create(raw, raw.size());
    };
    const auto table = [](u32 type, u32 flags, u32 space = 0, u32 vis = 0) {
        d3d12::RootParameter p;
        p.shader_visibility = vis;
        d3d12::RootDescriptorRange r;
        r.range_type = type;
        r.num_descriptors = 1;
        r.register_space = space;
        r.flags = flags;
        r.offset_in_descriptors_from_table_start = 0xFFFFFFFFu;
        p.ranges.push_back(r);
        return p;
    };
    const auto root = [](u32 type, u32 flags, u32 space = 0) {
        d3d12::RootParameter p;
        p.parameter_type = type;
        p.register_space = space;
        p.flags = flags;
        return p;
    };
    const auto consts = [](u32 reg, u32 space, u32 count) {
        d3d12::RootParameter p;
        p.parameter_type = 1;
        p.shader_register = reg;
        p.register_space = space;
        p.num_32bit_values = count;
        return p;
    };
    const auto sampler = [](u32 flags) {
        d3d12::StaticSampler s;
        s.filter = 0x15;
        s.address_u = s.address_v = s.address_w = 1;
        s.max_anisotropy = 16;
        s.comparison_func = 8;
        s.max_lod = 3.402823466e+38f;
        s.flags = flags;
        return s;
    };
    const char* labels[] = {"rootsig_flags", "range_flags_srv", "range_flags_sampler",
                            "root_descriptor_flags", "sampler_flags"};
    for (int what = 0; what < 5; ++what) {
        std::string line = fmt("devscan %s ok:", labels[what]);
        for (u32 bit = 0; bit < 32; ++bit) {
            const u32 f = 1u << bit;
            d3d12::RootSignatureDesc d;
            u32 version = 2;
            switch (what) {
            case 0: d.flags = f; version = 1; break;
            case 1: d.parameters.push_back(table(0, f)); break;
            case 2: d.parameters.push_back(table(3, f)); break;
            case 3: d.parameters.push_back(root(2, f)); break;
            case 4: d.static_samplers.push_back(sampler(f)); version = 3; break;
            }
            d.version = version;
            if (raw_hr(d, version) == 0) line += fmt(" %X", f);
        }
        actual.push_back(line);
    }
    for (u32 flags : {0x3u, 0x5u, 0x9u, 0x11u, 0x12u, 0x18u, 0x10010u, 0x10009u}) {
        for (u32 type : {0u, 3u}) {
            d3d12::RootSignatureDesc d;
            d.version = 2;
            d.parameters.push_back(table(type, flags));
            actual.push_back(fmt("devscan range_flags_%s 0x%X hr=0x%08X",
                                 type == 0 ? "srv" : "sampler", flags, raw_hr(d, 2)));
        }
    }
    for (u32 flags : {0x3u, 0x11u, 0x12u, 0x16u, 0x1Au}) {
        d3d12::RootSignatureDesc d;
        d.version = 2;
        d.parameters.push_back(root(2, flags));
        actual.push_back(fmt("devscan root_descriptor_flags 0x%X hr=0x%08X", flags,
                             raw_hr(d, 2)));
    }
    const auto v1 = [&](std::initializer_list<d3d12::RootParameter> params,
                        u32 flags = 0,
                        std::initializer_list<d3d12::StaticSampler> samplers = {}) {
        d3d12::RootSignatureDesc d;
        d.parameters = params;
        d.static_samplers = samplers;
        d.flags = flags;
        return raw_hr(d, 1);
    };
    d3d12::StaticSampler bad_filter = sampler(0);
    bad_filter.filter = 3;
    actual.push_back(fmt("devscan cost65_vs_vis hr=0x%08X",
                         v1({consts(0, 0, 65), table(0, 0, 0, 8)})));
    actual.push_back(fmt("devscan vis_vs_cost65 hr=0x%08X",
                         v1({table(0, 0, 0, 8), consts(0, 0, 65)})));
    actual.push_back(fmt("devscan cost65_vs_reserved_space hr=0x%08X",
                         v1({consts(0, 0xFFFFFFF0u, 65)})));
    actual.push_back(fmt("devscan cost65_vs_sampler_filter hr=0x%08X",
                         v1({consts(0, 0, 65)}, 0, {bad_filter})));
    actual.push_back(fmt("devscan cost65_local hr=0x%08X", v1({consts(0, 0, 65)}, 0x80)));
    for (u32 flags : {0x1080u, 0x8080u, 0x80000080u, 0x10080u}) {
        d3d12::RootSignatureDesc d;
        d.flags = flags;
        actual.push_back(fmt("devscan rootsig_flags 0x%X hr=0x%08X", flags, raw_hr(d, 1)));
    }
    actual.push_back(fmt("devscan cost64_plus_zero_consts hr=0x%08X",
                         v1({consts(0, 0, 0), consts(1, 0, 64)})));
    actual.push_back(fmt("devscan consts_ffffffff hr=0x%08X",
                         v1({consts(0, 0, 0xFFFFFFFFu)})));
    {
        d3d12::RootSignatureDesc d;
        d.parameters.resize(64);
        actual.push_back(fmt("devscan cost64_empty_tables hr=0x%08X", raw_hr(d, 1)));
        d.parameters.resize(65);
        actual.push_back(fmt("devscan cost65_empty_tables hr=0x%08X", raw_hr(d, 1)));
    }
    for (u32 space : {0xFFFFFFEFu, 0xFFFFFFF0u, 0xFFFFFFF8u, 0xFFFFFFFFu}) {
        actual.push_back(fmt("devscan rootsrv_space_%X hr=0x%08X", space,
                             v1({root(3, 0, space)})));
        actual.push_back(fmt("devscan sampler_table_space_%X hr=0x%08X", space,
                             v1({table(3, 0, space)})));
    }
    {
        d3d12::RootParameter wide = table(0, 0, 0xFFFFFFF0u);
        wide.ranges[0].num_descriptors = 4;
        actual.push_back(fmt("devscan reserved_space_vs_vis_p1 hr=0x%08X",
                             v1({wide, table(0, 0, 0, 8)})));
    }
    expect_same(expected, actual, "device [devscan]");
}

// The seam D12CORE uses, on Windows' own blobs: every blob the serializer
// produced parses, and serializing the parsed desc at its own version gives the
// same bytes back. (Blobs with root-signature flag bit 31 are skipped: the
// deserializer rule drops that bit, rts_flags_0x80000000.) The guest layout
// writer and reader agree at every version, in both bitnesses.
TEST(D3D12RootSignatureSeam, WindowsBlobsRoundTripThroughParseAndSerialize) {
    ProbeReplay probe(true);
    ASSERT_TRUE(probe.loaded());
    probe.ser();
    ASSERT_GT(probe.blobs().size(), 300u);
    usize checked = 0;
    for (const Stored& s : probe.blobs()) {
        if ((get32(s.bytes, 44 + 20) & 0x80000000u) != 0) continue;
        d3d12::RootSignatureDesc desc;
        ASSERT_EQ(d3d12::parse_root_signature_blob(s.bytes.data(), s.bytes.size(), desc),
                  d3d12::S_OK) << s.name;
        std::vector<u8> again;
        std::string message;
        const u32 hr = d3d12::serialize_root_signature(desc, desc.version, again, message);
        // Windows serialized it, so it validates -- except the one warning.
        EXPECT_EQ(hr, d3d12::S_OK) << s.name << ": " << message;
        EXPECT_EQ(again, s.bytes) << s.name;
        for (bool is_64bit : {true, false}) {
            GuestMemory memory;
            ASSERT_TRUE(memory.map_region(0x100000, 0x40000,
                                          MemoryProtection::ReadWrite).is_ok());
            for (u32 version = 1; version <= 3; ++version) {
                const auto converted = d3d12::convert_root_signature(desc, version);
                ASSERT_LE(d3d12::guest_root_signature_desc_size(converted, version,
                                                                is_64bit, false),
                          0x40000u);
                ASSERT_TRUE(d3d12::write_guest_root_signature_desc(
                    memory, 0x100000, converted, version, is_64bit, false));
                d3d12::RootSignatureDesc read;
                ASSERT_TRUE(d3d12::read_guest_root_signature_desc(
                    memory, 0x100000, version, is_64bit, read));
                std::vector<u8> a;
                std::vector<u8> b;
                std::string ma;
                std::string mb;
                (void)d3d12::serialize_root_signature(converted, version, a, ma);
                (void)d3d12::serialize_root_signature(read, version, b, mb);
                EXPECT_EQ(a, b) << s.name << " v" << version
                                << (is_64bit ? " x64" : " x86");
            }
        }
        ++checked;
    }
    EXPECT_GT(checked, 300u);
}

// The exports' x86 stack cleanup, from the SDK's x86 d3d12.lib decorations
// (_D3D12CreateRootSignatureDeserializer@16 and friends).
TEST(D3D12RootSignatureExports, StdcallBytesMatchTheImportLibrary) {
    NullBackend backend;
    GuestMemory memory;
    D3D12Manager manager(memory, false, backend);
    const DllExports exports = create_d3d12_thunks(manager);
    const auto bytes_of = [&](const std::string& name) -> int {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) return entry.stdcall_bytes;
        }
        return -1;
    };
    EXPECT_EQ(bytes_of("D3D12SerializeRootSignature"), 16);
    EXPECT_EQ(bytes_of("D3D12SerializeVersionedRootSignature"), 12);
    EXPECT_EQ(bytes_of("D3D12CreateRootSignatureDeserializer"), 16);
    EXPECT_EQ(bytes_of("D3D12CreateVersionedRootSignatureDeserializer"), 16);
    for (usize slot = 0; slot < d3d12::BLOB_METHOD_COUNT; ++slot) {
        EXPECT_EQ(bytes_of(d3d12_method_export_name("Blob", slot)),
                  d3d12::BLOB_STDCALL_BYTES[slot]);
    }
    for (usize slot = 0; slot < d3d12::ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT;
         ++slot) {
        EXPECT_EQ(bytes_of(d3d12_method_export_name("RootSignatureDeserializer", slot)),
                  d3d12::ROOT_SIGNATURE_DESERIALIZER_STDCALL_BYTES[slot]);
    }
    for (usize slot = 0;
         slot < d3d12::VERSIONED_ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT; ++slot) {
        EXPECT_EQ(bytes_of(d3d12_method_export_name(
                      "VersionedRootSignatureDeserializer", slot)),
                  d3d12::VERSIONED_ROOT_SIGNATURE_DESERIALIZER_STDCALL_BYTES[slot]);
    }
}

} // namespace
} // namespace vitreo::winapi
