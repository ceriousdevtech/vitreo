// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12.cpp
/// @brief d3d12.dll — the object model, memory, descriptors and submission,
/// driven through the guest-visible vtables on x64 and x86.
///
/// Expected values come from Learn or from Windows measurements
/// (tools/oracle_probe/d12core_facts_probe.expected), never from running this
/// implementation.

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {
namespace {

using namespace d3d12;

// =============================================================================
// A backend with host memory for heaps and recorders for everything else
// =============================================================================

class FakeBackend final : public graphics::GraphicsBackendDevice {
public:
    class FakeBuffer final : public graphics::BackendBuffer {
    public:
        // Page-aligned, as a CPU-visible backend heap's host memory is: the
        // frontend maps it into the guest.
        explicit FakeBuffer(usize size)
            : m_storage(static_cast<u8*>(std::aligned_alloc(
                            0x10000, (size + 0xFFFFu) & ~usize{0xFFFFu})),
                        &std::free),
              m_size(size) {
            std::memset(m_storage.get(), 0, (size + 0xFFFFu) & ~usize{0xFFFFu});
            m_desc.byte_width = size;
        }
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] const graphics::BackendBufferDesc& desc()
            const noexcept override {
            return m_desc;
        }
        [[nodiscard]] bool update(usize offset, const void* data, usize length,
                                  graphics::BufferWrite) override {
            if (offset + length > m_size) return false;
            std::memcpy(m_storage.get() + offset, data, length);
            return true;
        }
        [[nodiscard]] u8* bytes() { return m_storage.get(); }

    private:
        graphics::BackendBufferDesc m_desc{};
        std::unique_ptr<u8, decltype(&std::free)> m_storage;
        usize m_size{0};
    };

    class FakeHeap final : public graphics::BackendHeap {
    public:
        FakeHeap(u64 size, bool cpu) : m_buffer(static_cast<usize>(size)) {
            m_size = size;
            m_cpu = cpu;
        }
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] u64 size() const noexcept override { return m_size; }
        [[nodiscard]] void* host_pointer() const noexcept override {
            return m_cpu ? const_cast<FakeBuffer&>(m_buffer).bytes() : nullptr;
        }
        [[nodiscard]] graphics::BackendBuffer* buffer() noexcept override {
            return &m_buffer;
        }

    private:
        FakeBuffer m_buffer;
        u64 m_size{0};
        bool m_cpu{false};
    };

    class FakeTexture final : public graphics::BackendTexture2D {
    public:
        explicit FakeTexture(const graphics::BackendTexture2DDesc& desc)
            : m_desc(desc) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] const graphics::BackendTexture2DDesc& desc()
            const noexcept override {
            return m_desc;
        }
        [[nodiscard]] bool clear_render_target(
            const std::array<float, 4>&) override {
            return true;
        }
        [[nodiscard]] bool clear_depth_stencil(float, u8) override {
            return true;
        }
        [[nodiscard]] bool update(unsigned, const void*, unsigned) override {
            return true;
        }

    private:
        graphics::BackendTexture2DDesc m_desc;
    };

    class FakeFence final : public graphics::BackendFence {
    public:
        FakeFence(FakeBackend& owner, u64 initial)
            : m_owner(owner), m_value(initial) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] u64 completed_value() const override { return m_value; }
        [[nodiscard]] bool signal(u64 value) override {
            m_owner.stream.push_back("signal " + std::to_string(value));
            // The fake "GPU" runs in stream order at once.
            m_value = value;
            return true;
        }
        [[nodiscard]] bool wait_gpu(u64 value) override {
            m_owner.stream.push_back("wait " + std::to_string(value));
            return true;
        }
        [[nodiscard]] bool signal_cpu(u64 value) override {
            m_value = value;
            return true;
        }
        [[nodiscard]] bool wait_cpu(u64 value, u64) override {
            return m_value >= value;
        }

    private:
        FakeBackend& m_owner;
        u64 m_value{0};
    };

    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] std::string backend_device_name() const override {
        return "Fake";
    }
    [[nodiscard]] void* native_device() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] void* native_command_queue() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendBuffer> create_buffer(
        const graphics::BackendBufferDesc& desc, const void*) override {
        return std::make_unique<FakeBuffer>(desc.byte_width);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendTexture2D>
    create_texture_2d(const graphics::BackendTexture2DDesc& desc, const void*,
                      unsigned) override {
        return std::make_unique<FakeTexture>(desc);
    }
    /// A presenter that records what it shows into the stream.
    class FakeSwapChain final : public graphics::BackendSwapChain {
    public:
        FakeSwapChain(FakeBackend& owner, const graphics::BackendSwapChainDesc& d)
            : m_owner(owner), m_desc(d) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] const graphics::BackendSwapChainDesc& desc()
            const noexcept override {
            return m_desc;
        }
        [[nodiscard]] bool clear_back_buffer(
            const std::array<float, 4>&) override {
            return true;
        }
        [[nodiscard]] bool present(unsigned, unsigned) override { return true; }
        [[nodiscard]] bool present_texture(graphics::BackendTexture2D* source,
                                           unsigned, unsigned) override {
            m_owner.stream.push_back(
                "present " + std::to_string(source->desc().width) + "x" +
                std::to_string(source->desc().height));
            return true;
        }
        [[nodiscard]] bool resize(unsigned width, unsigned height,
                                  graphics::PixelFormat, unsigned) override {
            m_desc.width = width;
            m_desc.height = height;
            return true;
        }
        [[nodiscard]] std::unique_ptr<graphics::BackendRenderPass>
        begin_render_pass(bool, bool, float,
                          const std::array<graphics::BackendTexture2D*,
                                           graphics::BACKEND_MAX_RENDER_TARGETS>&)
            override {
            return nullptr;
        }

    private:
        FakeBackend& m_owner;
        graphics::BackendSwapChainDesc m_desc;
    };
    [[nodiscard]] std::unique_ptr<graphics::BackendSwapChain> create_swap_chain(
        const graphics::BackendSwapChainDesc& desc) override {
        return std::make_unique<FakeSwapChain>(*this, desc);
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
    [[nodiscard]] bool copy_buffer_region(graphics::BackendBuffer* dst,
                                          u64 dst_offset,
                                          const graphics::BackendBuffer* src,
                                          u64 src_offset, u64 bytes) override {
        stream.push_back("copy " + std::to_string(bytes));
        auto* d = static_cast<FakeBuffer*>(dst);
        auto* s = static_cast<FakeBuffer*>(const_cast<graphics::BackendBuffer*>(
            src));
        std::memcpy(d->bytes() + dst_offset, s->bytes() + src_offset,
                    static_cast<usize>(bytes));
        return true;
    }
    [[nodiscard]] bool copy_texture_2d(
        graphics::BackendTexture2D*,
        const graphics::BackendTexture2D*) override {
        return false;
    }
    [[nodiscard]] bool generate_mips(graphics::BackendTexture2D*) override {
        return false;
    }
    void flush() override { ++flushes; }
    [[nodiscard]] bool supports_fences() const noexcept override {
        return true;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendFence> create_fence(
        u64 initial) override {
        return std::make_unique<FakeFence>(*this, initial);
    }
    [[nodiscard]] bool supports_placement_heaps() const noexcept override {
        return true;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendHeap> create_heap(
        const graphics::BackendHeapDesc& desc) override {
        ++heaps;
        return std::make_unique<FakeHeap>(desc.size, desc.cpu_visible);
    }
    [[nodiscard]] bool heap_texture_placement(
        const graphics::BackendTexture2DDesc& desc, u64& size,
        u64& alignment) override {
        size = static_cast<u64>(desc.width) * desc.height * 4u;
        alignment = 4096;
        return true;
    }

    std::vector<std::string> stream;
    unsigned flushes{0};
    unsigned heaps{0};
};

// =============================================================================
// Calls through the vtables, x64 or x86
// =============================================================================

constexpr GuestAddr kData = 0x40000;
constexpr usize kDataSize = 0x40000;
constexpr GuestAddr kStack = kData + kDataSize - 0x1000;
constexpr GuestAddr kIid = kData + 0x100;
constexpr GuestAddr kOut = kData + 0x200;
constexpr GuestAddr kOut2 = kData + 0x240;
constexpr GuestAddr kDesc = kData + 0x400;
constexpr GuestAddr kDesc2 = kData + 0x800;
constexpr GuestAddr kArray = kData + 0xC00;

class Harness {
public:
    explicit Harness(bool is_64bit) : m_is_64bit(is_64bit) {
        EXPECT_TRUE(memory.map_region(kData, kDataSize,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.mode = is_64bit ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        manager = std::make_unique<D3D12Manager>(memory, is_64bit, backend);
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
        fill("VersionedRootSignatureDeserializer",
             thunks.versioned_root_signature_deserializer);
        manager->set_method_thunks(thunks);
    }

    [[nodiscard]] u32 ptr_size() const { return m_is_64bit ? 8u : 4u; }
    [[nodiscard]] u64 read_ptr(GuestAddr at) const {
        return m_is_64bit ? memory.read_u64(at).value()
                          : memory.read_u32(at).value();
    }
    void write_ptr(GuestAddr at, u64 value) {
        EXPECT_TRUE((m_is_64bit ? memory.write_u64(at, value)
                                : memory.write_u32(at, static_cast<u32>(value)))
                        .is_ok());
    }
    void w32(GuestAddr at, u32 v) { EXPECT_TRUE(memory.write_u32(at, v).is_ok()); }
    void w64(GuestAddr at, u64 v) { EXPECT_TRUE(memory.write_u64(at, v).is_ok()); }
    [[nodiscard]] u32 r32(GuestAddr at) const { return memory.read_u32(at).value(); }
    [[nodiscard]] u64 r64(GuestAddr at) const { return memory.read_u64(at).value(); }

    /// One argument as the guest passes it: a UINT64 (or 8-byte struct) is
    /// one slot on x64 and two on x86.
    struct Arg {
        u64 value;
        bool wide{false};
    };
    [[nodiscard]] static Arg wide(u64 v) { return {v, true}; }

    /// Call export @p name; the result as the guest sees it (EAX on x86, or
    /// EDX:EAX with @p wide_return).
    u64 call(const std::string& name, const std::vector<Arg>& args,
             bool wide_return = false) {
        const auto found = by_name.find(name);
        EXPECT_NE(found, by_name.end()) << name;
        if (found == by_name.end()) return ~0ull;
        return invoke(found->second->handler, args, wide_return,
                      found->second->stdcall_bytes, name);
    }

    /// Call a DXGI entry point the runtime routes to this manager (a
    /// factory method whose pDevice is a D3D12 queue).
    u64 call_manager(void (D3D12Manager::*entry)(Marshaler&),
                     const std::vector<Arg>& args) {
        return invoke([&](Marshaler& m) { (manager.get()->*entry)(m); }, args,
                      false, 0, "");
    }

    template <typename Handler>
    u64 invoke(const Handler& handler, const std::vector<Arg>& args,
               bool wide_return, usize stdcall_bytes, const std::string& name) {
        cpu.gpr.rsp = kStack;
        cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        if (!m_is_64bit) cpu.gpr.rdx = 0xCCCCCCCCCCCCCCCCull;
        if (m_is_64bit) {
            u64* regs[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) {
                    *regs[i] = args[i].value;
                } else {
                    EXPECT_TRUE(memory.write_u64(kStack + 0x28 + (i - 4) * 8,
                                                 args[i].value).is_ok());
                }
            }
        } else {
            usize slot = 0;
            for (const Arg& a : args) {
                EXPECT_TRUE(memory.write_u32(kStack + 4 + slot * 4,
                                             static_cast<u32>(a.value)).is_ok());
                ++slot;
                if (a.wide) {
                    EXPECT_TRUE(memory.write_u32(
                        kStack + 4 + slot * 4,
                        static_cast<u32>(a.value >> 32)).is_ok());
                    ++slot;
                }
            }
            // The callee pops exactly what the SDK prototype occupies.
            if (!name.empty()) EXPECT_EQ(stdcall_bytes, slot * 4) << name;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        handler(m);
        if (m_is_64bit) return cpu.gpr.rax;
        if (wide_return) {
            return (cpu.gpr.rax & 0xFFFFFFFFull) |
                   ((cpu.gpr.rdx & 0xFFFFFFFFull) << 32);
        }
        return cpu.gpr.rax & 0xFFFFFFFFull;
    }

    /// Slot @p slot of the COM object at @p object, through its vtable.
    u64 com(GuestAddr object, usize slot, std::vector<Arg> args,
            bool wide_return = false) {
        const u64 vtable = read_ptr(object);
        EXPECT_NE(vtable, 0u) << "object 0x" << std::hex << object;
        const u64 thunk = read_ptr(vtable + slot * ptr_size());
        const auto name = by_thunk.find(thunk);
        EXPECT_NE(name, by_thunk.end()) << "vtable slot " << slot;
        if (name == by_thunk.end()) return ~0ull;
        args.insert(args.begin(), Arg{object});
        return call(name->second, args, wide_return);
    }

    GuestAddr iid(const GuidBytes& value) {
        EXPECT_TRUE(memory.write_bytes(kIid, value.data(), value.size()).is_ok());
        return kIid;
    }

    GuestAddr device(u32 level = D3D_FEATURE_LEVEL_11_0) {
        const u64 hr = call("D3D12CreateDevice",
                            {{0}, {level}, {iid(IID_ID3D12DEVICE)}, {kOut}});
        EXPECT_EQ(hr, S_OK);
        return read_ptr(kOut);
    }

    GuestAddr queue(GuestAddr dev, u32 type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
        const auto& l = layout::D3D12_COMMAND_QUEUE_DESC[m_is_64bit];
        for (u32 i = 0; i < l.size; i += 4) w32(kDesc + i, 0);
        w32(kDesc + l.Type, type);
        EXPECT_EQ(com(dev, DeviceSlot::CreateCommandQueue,
                      {{kDesc}, {iid(IID_ID3D12COMMANDQUEUE)}, {kOut}}),
                  S_OK);
        return read_ptr(kOut);
    }

    GuestAddr allocator(GuestAddr dev, u32 type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
        EXPECT_EQ(com(dev, DeviceSlot::CreateCommandAllocator,
                      {{type}, {iid(IID_ID3D12COMMANDALLOCATOR)}, {kOut}}),
                  S_OK);
        return read_ptr(kOut);
    }

    u64 try_list(GuestAddr dev, GuestAddr alloc, GuestAddr& out,
                 u32 type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
        const u64 hr = com(dev, DeviceSlot::CreateCommandList,
                           {{0}, {type}, {alloc}, {0},
                            {iid(IID_ID3D12GRAPHICSCOMMANDLIST)}, {kOut}});
        out = read_ptr(kOut);
        return hr;
    }

    GuestAddr fence(GuestAddr dev, u64 initial) {
        EXPECT_EQ(com(dev, DeviceSlot::CreateFence,
                      {wide(initial), {0}, {iid(IID_ID3D12FENCE)}, {kOut}}),
                  S_OK);
        return read_ptr(kOut);
    }

    /// CreateCommittedResource of a buffer.
    u64 buffer(GuestAddr dev, u32 heap_type, u64 width, u32 state,
               GuestAddr& out, u32 flags = 0) {
        const auto& hp = layout::D3D12_HEAP_PROPERTIES[m_is_64bit];
        const auto& rd = layout::D3D12_RESOURCE_DESC[m_is_64bit];
        for (u32 i = 0; i < hp.size; i += 4) w32(kDesc + i, 0);
        for (u32 i = 0; i < rd.size; i += 4) w32(kDesc2 + i, 0);
        w32(kDesc + hp.Type, heap_type);
        w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
        w64(kDesc2 + rd.Width, width);
        w32(kDesc2 + rd.Height, 1);
        EXPECT_TRUE(memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
        EXPECT_TRUE(memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
        w32(kDesc2 + rd.SampleDesc_Count, 1);
        w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
        w32(kDesc2 + rd.Flags, flags);
        const u64 hr = com(dev, DeviceSlot::CreateCommittedResource,
                           {{kDesc}, {0}, {kDesc2}, {state}, {0},
                            {iid(IID_ID3D12RESOURCE)}, {kOut}});
        out = read_ptr(kOut);
        return hr;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    FakeBackend backend;
    std::unique_ptr<D3D12Manager> manager;
    DllExports exports;
    std::unordered_map<std::string, const ExportEntry*> by_name;
    std::unordered_map<u64, std::string> by_thunk;
    bool m_is_64bit;
};

/// Every D3D12 test runs on both bitnesses.
class D3D12Test : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (!d3d12_enabled()) {
            GTEST_SKIP() << "set VITREO_D3D12=1 to exercise the device";
        }
        h = std::make_unique<Harness>(GetParam());
    }
    std::unique_ptr<Harness> h;
};

INSTANTIATE_TEST_SUITE_P(Bitness, D3D12Test, ::testing::Values(true, false),
                         [](const auto& info) {
                             return info.param ? "x64" : "x86";
                         });

// =============================================================================
// Module exports and the opt-in gate
// =============================================================================

// Ordinals from the real d3d12.dll's export table (ordinal base 99): the
// SDK's d3d12.lib binds D3D12CreateDevice and D3D12GetDebugInterface BY
// ORDINAL. Stack cleanup from the x86 import library's decorations
// (libd3d12.a: _D3D12CreateDevice@16, _D3D12EnableExperimentalFeatures@16,
// _D3D12GetDebugInterface@8, ...).
TEST(D3D12Exports, OrdinalsAndStdcallBytesMatchTheImportLibrary) {
    FakeBackend backend;
    GuestMemory memory;
    D3D12Manager manager(memory, false, backend);
    const DllExports exports = create_d3d12_thunks(manager);
    const auto find = [&](std::string_view name) -> const ExportEntry* {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    };
    struct Expect {
        const char* name;
        int ordinal;
        int bytes;
    };
    for (const Expect& e : {Expect{"D3D12CreateDevice", 101, 16},
                            Expect{"D3D12GetDebugInterface", 102, 8},
                            Expect{"D3D12CreateRootSignatureDeserializer", 106, 16},
                            Expect{"D3D12CreateVersionedRootSignatureDeserializer",
                                   107, 16},
                            Expect{"D3D12EnableExperimentalFeatures", 109, 16},
                            Expect{"D3D12SerializeRootSignature", 115, 16},
                            Expect{"D3D12SerializeVersionedRootSignature", 116,
                                   12}}) {
        const ExportEntry* entry = find(e.name);
        ASSERT_NE(entry, nullptr) << e.name;
        EXPECT_EQ(entry->ordinal, e.ordinal) << e.name;
        EXPECT_EQ(entry->stdcall_bytes, e.bytes) << e.name;
    }
    // Every vtable slot is an export with the generated cleanup.
    for (usize table = 0; table < INTERFACE_TABLE_COUNT; ++table) {
        const auto& row = INTERFACE_TABLES[table];
        for (usize slot = 0; slot < row.method_count; ++slot) {
            const ExportEntry* entry =
                find(d3d12_method_export_name(row.export_name, slot));
            ASSERT_NE(entry, nullptr) << row.export_name << " " << slot;
            EXPECT_EQ(entry->stdcall_bytes, row.stdcall_bytes[slot]);
        }
    }
}

// A struct passed by value counts its size on the x86 stack: the SDK's
// D3D12_GPU_DESCRIPTOR_HANDLE is { UINT64 ptr }, so
// SetGraphicsRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) pops 4 +
// 4 + 8 bytes, and ClearUnorderedAccessViewUint (GPU handle, CPU handle,
// resource, values, count, rects) 4 + 8 + 5 * 4.
TEST(D3D12Exports, ByValueHandlesCountTheirSize) {
    EXPECT_EQ(GRAPHICS_COMMAND_LIST_STDCALL_BYTES
                  [GraphicsCommandListSlot::SetGraphicsRootDescriptorTable],
              16u);
    EXPECT_EQ(GRAPHICS_COMMAND_LIST_STDCALL_BYTES
                  [GraphicsCommandListSlot::ClearUnorderedAccessViewUint],
              32u);
    // D3D12_CPU_DESCRIPTOR_HANDLE { SIZE_T ptr } is four bytes on x86.
    EXPECT_EQ(DEVICE_STDCALL_BYTES[DeviceSlot::CreateSampler], 12u);
}

TEST(D3D12Gate, DeviceCreationIsOffByDefaultAndReportsUnsupported) {
    if (d3d12_enabled()) GTEST_SKIP() << "VITREO_D3D12 is set";
    Harness h(true);
    h.w64(kOut, 0xDEADBEEFull);
    EXPECT_EQ(h.call("D3D12CreateDevice",
                     {{0}, {D3D_FEATURE_LEVEL_11_0}, {h.iid(IID_ID3D12DEVICE)},
                      {kOut}}),
              DXGI_ERROR_UNSUPPORTED)
        << "the guest needs DXGI_ERROR_UNSUPPORTED to fall back";
    EXPECT_EQ(h.r64(kOut), 0u);
}

// =============================================================================
// Object model (d12core_facts_probe)
// =============================================================================

// A child holds a reference on its device; D3D12CreateDevice returns the
// existing device (Learn: devices are singletons per adapter).
TEST_P(D3D12Test, ChildrenReferenceTheSingletonDevice) {
    const GuestAddr dev = h->device();
    EXPECT_EQ(h->com(dev, 1, {}), 2u);  // device_addref 2
    EXPECT_EQ(h->com(dev, 2, {}), 1u);  // device_release 1
    const GuestAddr queue = h->queue(dev);
    EXPECT_EQ(h->com(dev, 1, {}), 3u);  // device_addref_with_queue 3
    EXPECT_EQ(h->com(dev, 2, {}), 2u);
    // GetDevice hands the same device back with a reference.
    EXPECT_EQ(h->com(queue, 7, {{h->iid(IID_ID3D12DEVICE)}, {kOut2}}), S_OK);
    EXPECT_EQ(h->read_ptr(kOut2), dev);
    EXPECT_EQ(h->com(dev, 2, {}), 2u);
    // second_device_same 1, second_device_release 2.
    EXPECT_EQ(h->device(), dev);
    EXPECT_EQ(h->com(dev, 2, {}), 2u);
    EXPECT_EQ(h->com(queue, 2, {}), 0u);
    EXPECT_EQ(h->com(dev, 2, {}), 0u);
}

TEST_P(D3D12Test, SetNameIsTheWideDebugNamePrivateData) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr name = kData + 0x3000;
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(h->memory.write_u16(name + i * 2, i < 3 ? 'a' + i : 0).is_ok());
    }
    EXPECT_EQ(h->com(queue, 6, {{name}}), S_OK);
    ASSERT_TRUE(h->memory.write_bytes(kDesc, WKPDID_D3DDEBUGOBJECTNAMEW.data(),
                                      16).is_ok());
    h->w32(kOut2, 0);
    // setname_private hr=0 size=8 ("abc" and its terminator, UTF-16).
    EXPECT_EQ(h->com(queue, 3, {{kDesc}, {kOut2}, {0}}), S_OK);
    EXPECT_EQ(h->r32(kOut2), 8u);
}

TEST_P(D3D12Test, CustomHeapPropertiesAreTheCacheCoherentUmaOnes) {
    const GuestAddr dev = h->device();
    const auto& l = layout::D3D12_HEAP_PROPERTIES[GetParam()];
    struct Row {
        u32 type, cpu, pool;
    };
    // custom_heap 1/2/3: DEFAULT NOT_AVAILABLE, UPLOAD/READBACK WRITE_BACK,
    // all L0, node masks 1.
    for (const Row& row : {Row{1, 1, 1}, Row{2, 3, 1}, Row{3, 3, 1}}) {
        EXPECT_EQ(h->com(dev, DeviceSlot::GetCustomHeapProperties,
                         {{kDesc}, {0}, {row.type}}),
                  GetParam() ? kDesc : static_cast<u32>(kDesc));
        EXPECT_EQ(h->r32(kDesc + l.Type), D3D12_HEAP_TYPE_CUSTOM);
        EXPECT_EQ(h->r32(kDesc + l.CPUPageProperty), row.cpu);
        EXPECT_EQ(h->r32(kDesc + l.MemoryPoolPreference), row.pool);
        EXPECT_EQ(h->r32(kDesc + l.CreationNodeMask), 1u);
        EXPECT_EQ(h->r32(kDesc + l.VisibleNodeMask), 1u);
    }
}

// =============================================================================
// Descriptor heaps
// =============================================================================

TEST_P(D3D12Test, DescriptorHeapHandlesUseTheHiddenReturnPointer) {
    const GuestAddr dev = h->device();
    const auto& l = layout::D3D12_DESCRIPTOR_HEAP_DESC[GetParam()];
    for (u32 visible = 0; visible < 2; ++visible) {
        h->w32(kDesc + l.Type, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        h->w32(kDesc + l.NumDescriptors, 16);
        h->w32(kDesc + l.Flags,
               visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : 0u);
        h->w32(kDesc + l.NodeMask, 0);
        ASSERT_EQ(h->com(dev, DeviceSlot::CreateDescriptorHeap,
                         {{kDesc}, {h->iid(IID_ID3D12DESCRIPTORHEAP)}, {kOut}}),
                  S_OK);
        const GuestAddr heap = h->read_ptr(kOut);
        // The CPU handle is a SIZE_T: poison eight bytes and see exactly the
        // pointer width written, and RetVal returned.
        h->w64(kOut2, 0xA5A5A5A5A5A5A5A5ull);
        EXPECT_EQ(h->com(heap, DescriptorHeapSlot::GetCPUDescriptorHandleForHeapStart,
                         {{kOut2}}),
                  GetParam() ? kOut2 : static_cast<u32>(kOut2));
        EXPECT_NE(h->read_ptr(kOut2), 0u);
        if (!GetParam()) EXPECT_EQ(h->r32(kOut2 + 4), 0xA5A5A5A5u);
        // cpu_nonzero=1 gpu_nonzero=1 for both heaps (measured).
        h->w64(kOut2, 0);
        EXPECT_EQ(h->com(heap, DescriptorHeapSlot::GetGPUDescriptorHandleForHeapStart,
                         {{kOut2}}),
                  GetParam() ? kOut2 : static_cast<u32>(kOut2));
        EXPECT_NE(h->r64(kOut2), 0u);
    }
    // rtv_heap_shader_visible 0x80070057.
    h->w32(kDesc + l.Type, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    h->w32(kDesc + l.Flags, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateDescriptorHeap,
                     {{kDesc}, {h->iid(IID_ID3D12DESCRIPTORHEAP)}, {kOut}}),
              E_INVALIDARG);
}

// =============================================================================
// Resources and memory
// =============================================================================

// An UPLOAD buffer is mapped once, persistently: every Map hands out the same
// pointer (map_same_pointer, map_again_same_pointer), a buffer has one
// subresource (map_subresource1 E_INVALIDARG), and the guest's bytes are the
// memory the GPU reads -- no copy between them.
TEST_P(D3D12Test, UploadBuffersMapPersistentlyAndZeroCopy) {
    const GuestAddr dev = h->device();
    GuestAddr buffer = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 1000,
                        D3D12_RESOURCE_STATE_GENERIC_READ, buffer),
              S_OK);
    EXPECT_EQ(h->com(buffer, ResourceSlot::Map, {{0}, {0}, {0}}), S_OK);
    ASSERT_EQ(h->com(buffer, ResourceSlot::Map, {{0}, {0}, {kOut}}), S_OK);
    const GuestAddr a = h->read_ptr(kOut);
    ASSERT_EQ(h->com(buffer, ResourceSlot::Map, {{0}, {0}, {kOut}}), S_OK);
    EXPECT_EQ(h->read_ptr(kOut), a);
    for (int i = 0; i < 3; ++i) (void)h->com(buffer, ResourceSlot::Unmap, {{0}, {0}});
    ASSERT_EQ(h->com(buffer, ResourceSlot::Map, {{0}, {0}, {kOut}}), S_OK);
    EXPECT_EQ(h->read_ptr(kOut), a);
    EXPECT_EQ(h->com(buffer, ResourceSlot::Map, {{1}, {0}, {kOut}}),
              E_INVALIDARG);
    // The guest's write lands in the backend heap's host memory.
    ASSERT_TRUE(h->memory.write_u32(a + 12, 0x12345678u).is_ok());
    ASSERT_NE(h->backend.heaps, 0u);
    // GPU VA: non-zero and 64 KiB aligned (upload_va_nonzero, va_mod_64k 0).
    const u64 va = h->com(buffer, ResourceSlot::GetGPUVirtualAddress, {}, true);
    EXPECT_NE(va, 0u);
    EXPECT_EQ(va % 65536u, 0u);
    // GetDesc reports the 64 KiB alignment and ROW_MAJOR layout
    // (upload_desc alignment=65536 width=1000 layout=1).
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    EXPECT_EQ(h->com(buffer, ResourceSlot::GetDesc, {{kDesc}}),
              GetParam() ? kDesc : static_cast<u32>(kDesc));
    EXPECT_EQ(h->r64(kDesc + rd.Alignment), 65536u);
    EXPECT_EQ(h->r64(kDesc + rd.Width), 1000u);
    EXPECT_EQ(h->r32(kDesc + rd.Layout), D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    // upload_heap_props type=2 cpu=0 pool=0 create=1 visible=1 flags=0xc0.
    const auto& hp = layout::D3D12_HEAP_PROPERTIES[GetParam()];
    EXPECT_EQ(h->com(buffer, ResourceSlot::GetHeapProperties,
                     {{kDesc}, {kOut2}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc + hp.Type), D3D12_HEAP_TYPE_UPLOAD);
    EXPECT_EQ(h->r32(kDesc + hp.CPUPageProperty), 0u);
    EXPECT_EQ(h->r32(kDesc + hp.CreationNodeMask), 1u);
    EXPECT_EQ(h->r32(kOut2), 0xC0u);
}

TEST_P(D3D12Test, HeapTypesTakeTheMeasuredInitialStates) {
    const GuestAddr dev = h->device();
    GuestAddr out = 0;
    // upload_state_common 0, readback_state_generic_read 0x80070057,
    // readback_state_copy_dest 0.
    EXPECT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256,
                        D3D12_RESOURCE_STATE_COMMON, out),
              S_OK);
    EXPECT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_READBACK, 256,
                        D3D12_RESOURCE_STATE_GENERIC_READ, out),
              E_INVALIDARG);
    EXPECT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_READBACK, 256,
                        D3D12_RESOURCE_STATE_COPY_DEST, out),
              S_OK);
    // default_map 0x80070057, and *ppData left alone (default_map_out_null 0).
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
                        D3D12_RESOURCE_STATE_COMMON, out),
              S_OK);
    h->write_ptr(kOut2, 1);
    EXPECT_EQ(h->com(out, ResourceSlot::Map, {{0}, {0}, {kOut2}}),
              E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut2), 1u);
}

// A placed buffer lies inside its heap at a 64 KiB-aligned offset, and holds
// no COM reference on the heap (heap_addref_with_placed 2 after one create).
TEST_P(D3D12Test, PlacedResourcesLieInsideTheirHeap) {
    const GuestAddr dev = h->device();
    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    for (u32 i = 0; i < hd.size; i += 4) h->w32(kDesc + i, 0);
    h->w64(kDesc + hd.SizeInBytes, 1u << 20);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    h->w32(kDesc + hd.Flags, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateHeap,
                     {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}),
              S_OK);
    const GuestAddr heap = h->read_ptr(kOut);
    // heap_desc: size 1048576, alignment 65536 (0 in), node masks 1.
    EXPECT_EQ(h->com(heap, HeapSlot::GetDesc, {{kDesc2}}),
              GetParam() ? kDesc2 : static_cast<u32>(kDesc2));
    EXPECT_EQ(h->r64(kDesc2 + hd.Alignment), 65536u);
    EXPECT_EQ(h->r32(kDesc2 + hd.Properties_CreationNodeMask), 1u);
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    const GuestAddr desc = kData + 0x1000;
    for (u32 i = 0; i < rd.size; i += 4) h->w32(desc + i, 0);
    h->w32(desc + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
    h->w64(desc + rd.Width, 4096);
    h->w32(desc + rd.Height, 1);
    ASSERT_TRUE(h->memory.write_u16(desc + rd.DepthOrArraySize, 1).is_ok());
    ASSERT_TRUE(h->memory.write_u16(desc + rd.MipLevels, 1).is_ok());
    h->w32(desc + rd.SampleDesc_Count, 1);
    h->w32(desc + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    const auto place = [&](u64 offset) {
        return h->com(dev, DeviceSlot::CreatePlacedResource,
                      {{heap}, Harness::wide(offset), {desc},
                       {D3D12_RESOURCE_STATE_COMMON}, {0},
                       {h->iid(IID_ID3D12RESOURCE)}, {kOut}});
    };
    EXPECT_EQ(place(65536), S_OK);                    // placed_buffer
    EXPECT_EQ(h->com(heap, 1, {}), 2u);               // no reference taken
    EXPECT_EQ(h->com(heap, 2, {}), 1u);
    EXPECT_EQ(place(1000), E_INVALIDARG);             // placed_misaligned
    EXPECT_EQ(place((1u << 20) - 65536), S_OK);       // placed_last_64k
    EXPECT_EQ(place(1u << 20), E_INVALIDARG);         // placed_past_end
}

// =============================================================================
// Fences and queues
// =============================================================================

// A CPU Signal sets the value, even below the current one (fence_initial 7,
// fence_after_lower 3). GetCompletedValue is a UINT64 return: EDX:EAX on x86.
TEST_P(D3D12Test, FenceCpuSignalSetsTheValue) {
    const GuestAddr dev = h->device();
    const GuestAddr fence = h->fence(dev, 7);
    EXPECT_EQ(h->com(fence, FenceSlot::GetCompletedValue, {}, true), 7u);
    EXPECT_EQ(h->com(fence, FenceSlot::Signal, {Harness::wide(3)}), S_OK);
    EXPECT_EQ(h->com(fence, FenceSlot::GetCompletedValue, {}, true), 3u);
    EXPECT_EQ(h->com(fence, FenceSlot::Signal,
                     {Harness::wide(0x100000002ull)}),
              S_OK);
    EXPECT_EQ(h->com(fence, FenceSlot::GetCompletedValue, {}, true),
              0x100000002ull);
}

// A queue that waits for a value no signal has been submitted for holds its
// later work until one is: the backend stream never carries a wait ahead of
// the signal that releases it (d3d12_queue.cpp).
TEST_P(D3D12Test, CrossQueueWaitHoldsWorkUntilTheSignalIsSubmitted) {
    const GuestAddr dev = h->device();
    const GuestAddr direct = h->queue(dev);
    const GuestAddr copy = h->queue(dev, D3D12_COMMAND_LIST_TYPE_COPY);
    const GuestAddr fence = h->fence(dev, 0);
    EXPECT_EQ(h->com(direct, CommandQueueSlot::Wait,
                     {{fence}, Harness::wide(5)}),
              S_OK);
    EXPECT_EQ(h->com(direct, CommandQueueSlot::Signal,
                     {{fence}, Harness::wide(6)}),
              S_OK);
    EXPECT_TRUE(h->backend.stream.empty())
        << "nothing may reach the stream before the signal the wait needs";
    EXPECT_EQ(h->com(copy, CommandQueueSlot::Signal,
                     {{fence}, Harness::wide(5)}),
              S_OK);
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"signal 5", "wait 5", "signal 6"}));
}

// =============================================================================
// Command lists
// =============================================================================

TEST_P(D3D12Test, ListAndAllocatorFollowTheDocumentedStateRules) {
    const GuestAddr dev = h->device();
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    // Learn: an allocator backs one recording list at a time.
    GuestAddr second = 0;
    EXPECT_EQ(h->try_list(dev, alloc, second), E_INVALIDARG);
    // Reset of an allocator with a recording list: E_FAIL.
    EXPECT_EQ(h->com(alloc, CommandAllocatorSlot::Reset, {}), E_FAIL);
    // Reset of a recording list: E_FAIL; Close twice: E_FAIL.
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Reset, {{alloc}, {0}}),
              E_FAIL);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_FAIL);
    EXPECT_EQ(h->com(alloc, CommandAllocatorSlot::Reset, {}), S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Reset, {{alloc}, {0}}),
              S_OK);
    // A type mismatch between allocator and list.
    const GuestAddr compute = h->allocator(dev, D3D12_COMMAND_LIST_TYPE_COMPUTE);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Reset, {{compute}, {0}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::GetType, {}),
              D3D12_COMMAND_LIST_TYPE_DIRECT);
}

// A recording method that cannot be honoured records the error Close then
// returns, and such a list can never be reset (Learn, Close / Reset).
TEST_P(D3D12Test, RecordingErrorsSurfaceFromCloseAndReset) {
    const GuestAddr dev = h->device();
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    // CopyBufferRegion from a non-resource: an invalid argument.
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{0x1234}, Harness::wide(0), {0x5678}, Harness::wide(0),
                  Harness::wide(16)});
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Reset, {{alloc}, {0}}),
              E_INVALIDARG);
}

// CopyBufferRegion replays at ExecuteCommandLists, into the backend, between
// windows of the heaps' buffers: UPLOAD -> DEFAULT -> READBACK round trip.
TEST_P(D3D12Test, CopyBufferRegionRoundTripsThroughTheQueue) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr upload = 0, gpu = 0, readback = 0, list = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256,
                        D3D12_RESOURCE_STATE_GENERIC_READ, upload), S_OK);
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
                        D3D12_RESOURCE_STATE_COMMON, gpu), S_OK);
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_READBACK, 256,
                        D3D12_RESOURCE_STATE_COPY_DEST, readback), S_OK);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr src = h->read_ptr(kOut2);
    for (u32 i = 0; i < 16; ++i) h->w32(src + i * 4, 0xC0DE0000u + i);
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{gpu}, Harness::wide(16), {upload}, Harness::wide(0),
                  Harness::wide(64)});
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {gpu}, Harness::wide(16),
                  Harness::wide(64)});
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    // Nothing reaches the backend until the list executes.
    EXPECT_TRUE(h->backend.stream.empty());
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists,
                 {{1}, {kArray}});
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"copy 64", "copy 64"}));
    ASSERT_EQ(h->com(readback, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr dst = h->read_ptr(kOut2);
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(h->r32(dst + i * 4), 0xC0DE0000u + i) << i;
    }
}

// =============================================================================
// DXGI swap chains on a command queue
// =============================================================================

constexpr u64 kHwnd = 0x10044;
constexpr GuestAddr kChainDesc = kData + 0x1000;

/// Window services for one 120x48 window (the a08 probe's client area).
void attach_window(Harness& h) {
    D3D12Manager::PresentationServices services;
    services.presentation_view = [](u64 hwnd) -> void* {
        return hwnd == kHwnd ? reinterpret_cast<void*>(0x1) : nullptr;
    };
    services.client_size = [](u64 hwnd) -> std::optional<std::pair<u32, u32>> {
        if (hwnd != kHwnd) return std::nullopt;
        return std::pair<u32, u32>{120, 48};
    };
    h.manager->set_presentation_services(std::move(services));
}

/// DXGI_SWAP_CHAIN_DESC1 (48 bytes, no pointers).
void write_chain_desc(Harness& h, u32 width, u32 height, u32 count,
                      u32 flags = 0, u32 effect = 4 /* FLIP_DISCARD */) {
    const u32 values[12] = {width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                            1,     0,      0x20 /* RENDER_TARGET_OUTPUT */,
                            count, 0 /* STRETCH */, effect, 0, flags};
    for (u32 i = 0; i < 12; ++i) h.w32(kChainDesc + i * 4u, values[i]);
}

u64 create_chain(Harness& h, GuestAddr device_or_queue, GuestAddr& out) {
    h.write_ptr(kOut, 0xBAD0BAD0u);
    const u64 hr = h.call_manager(&D3D12Manager::impl_CreateSwapChainForHwnd,
                                  {{0}, {device_or_queue}, {kHwnd},
                                   {kChainDesc}, {0}, {0}, {kOut}});
    out = h.read_ptr(kOut);
    return hr;
}

// a08 create / buffers: a flip chain on a direct queue; the buffers are
// committed ID3D12Resource textures (flags ALLOW_RENDER_TARGET, heap flags
// 0x8d); a device is not a queue; one flip chain per window.
TEST_P(D3D12Test, SwapChainOnAQueueServesItsBuffersAsResources) {
    attach_window(*h);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    write_chain_desc(*h, 0, 0, 3);
    GuestAddr chain = 0;
    EXPECT_EQ(create_chain(*h, dev, chain), DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(chain, 0u);
    ASSERT_EQ(create_chain(*h, queue, chain), S_OK);
    ASSERT_NE(chain, 0u);
    GuestAddr second = 0;
    EXPECT_EQ(create_chain(*h, queue, second), E_ACCESSDENIED);
    EXPECT_EQ(second, 0u);
    // 0x0 takes the client area (size==client=1).
    ASSERT_EQ(h->com(chain, SwapChainSlot::GetDesc1, {{kDesc}}), S_OK);
    EXPECT_EQ(h->r32(kDesc + 0), 120u);
    EXPECT_EQ(h->r32(kDesc + 4), 48u);
    EXPECT_EQ(h->r32(kDesc + 28), 3u);
    std::array<GuestAddr, 3> buffers{};
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_EQ(h->com(chain, SwapChainSlot::GetBuffer,
                         {{i}, {h->iid(IID_ID3D12RESOURCE)}, {kOut}}),
                  S_OK);
        buffers[i] = h->read_ptr(kOut);
        ASSERT_NE(buffers[i], 0u);
    }
    EXPECT_NE(buffers[0], buffers[1]);
    EXPECT_NE(buffers[1], buffers[2]);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetBuffer,
                     {{3}, {h->iid(IID_ID3D12RESOURCE)}, {kOut}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->read_ptr(kOut), 0u);
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    (void)h->com(buffers[0], ResourceSlot::GetDesc, {{kDesc}});
    EXPECT_EQ(h->r32(kDesc + rd.Dimension), D3D12_RESOURCE_DIMENSION_TEXTURE2D);
    EXPECT_EQ(h->r64(kDesc + rd.Width), 120u);
    EXPECT_EQ(h->r32(kDesc + rd.Format),
              static_cast<u32>(DXGI_FORMAT_R8G8B8A8_UNORM));
    EXPECT_EQ(h->r32(kDesc + rd.Flags), 0x1u);
    const auto& hp = layout::D3D12_HEAP_PROPERTIES[GetParam()];
    ASSERT_EQ(h->com(buffers[0], ResourceSlot::GetHeapProperties,
                     {{kDesc}, {kOut2}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc + hp.Type), D3D12_HEAP_TYPE_DEFAULT);
    EXPECT_EQ(h->r32(kOut2), 0x8Du);
    // GetDevice answers the queue's device, not the queue.
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetDevice,
                     {{h->iid(IID_ID3D12DEVICE)}, {kOut}}),
              S_OK);
    EXPECT_EQ(h->read_ptr(kOut), dev);
    (void)h->com(dev, DeviceSlot::Release, {});
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetDevice,
                     {{h->iid(IID_ID3D12COMMANDQUEUE)}, {kOut}}),
              E_NOINTERFACE);
    // Each GetBuffer reference is the application's; the chain keeps one.
    for (const GuestAddr buffer : buffers) {
        EXPECT_EQ(h->com(buffer, ResourceSlot::Release, {}), 1u);
    }
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);
}

// a08 present: the index walks the ring; TEST moves nothing; a present held
// behind a queue wait is shown after the work that releases it.
TEST_P(D3D12Test, PresentRotatesTheBackBufferInQueueOrder) {
    attach_window(*h);
    const GuestAddr dev = h->device();
    const GuestAddr direct = h->queue(dev);
    const GuestAddr copy = h->queue(dev, D3D12_COMMAND_LIST_TYPE_COPY);
    const GuestAddr fence = h->fence(dev, 0);
    write_chain_desc(*h, 64, 48, 2);
    GuestAddr chain = 0;
    ASSERT_EQ(create_chain(*h, direct, chain), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 1u);
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"present 64x48"}));
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0x1 /* TEST */}}),
              S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 1u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{5}, {0}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present,
                     {{0}, {0x200 /* ALLOW_TEARING */}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0x80000000u}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->backend.stream.size(), 1u);
    // Held behind a wait no signal has been submitted for.
    EXPECT_EQ(h->com(direct, CommandQueueSlot::Wait,
                     {{fence}, Harness::wide(5)}),
              S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{1}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    EXPECT_EQ(h->backend.stream.size(), 1u);
    EXPECT_EQ(h->com(copy, CommandQueueSlot::Signal,
                     {{fence}, Harness::wide(5)}),
              S_OK);
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"present 64x48", "signal 5", "wait 5",
                                        "present 64x48"}));
    ASSERT_EQ(h->com(chain, SwapChainSlot::GetLastPresentCount, {{kOut2}}),
              S_OK);
    EXPECT_EQ(h->r32(kOut2), 2u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);
}

// a08 resize: every buffer reference must be released first; zero keeps the
// count and format, 0x0 takes the client area; one buffer, or adding
// ALLOW_TEARING, is E_INVALIDARG. The chain's queue reference goes with it.
TEST_P(D3D12Test, ResizeBuffersNeedsEveryBufferReferenceReleased) {
    attach_window(*h);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const u64 queue_refs = h->com(queue, CommandQueueSlot::AddRef, {});
    (void)h->com(queue, CommandQueueSlot::Release, {});
    write_chain_desc(*h, 64, 48, 2);
    GuestAddr chain = 0;
    ASSERT_EQ(create_chain(*h, queue, chain), S_OK);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::AddRef, {}), queue_refs + 1);
    (void)h->com(queue, CommandQueueSlot::Release, {});
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    ASSERT_EQ(h->com(chain, SwapChainSlot::GetBuffer,
                     {{0}, {h->iid(IID_ID3D12RESOURCE)}, {kOut}}),
              S_OK);
    const GuestAddr held = h->read_ptr(kOut);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {32}, {16}, {0}, {0}}),
              DXGI_ERROR_INVALID_CALL);
    (void)h->com(held, ResourceSlot::Release, {});
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {32}, {16}, {0}, {0}}),
              S_OK);
    ASSERT_EQ(h->com(chain, SwapChainSlot::GetDesc1, {{kDesc}}), S_OK);
    EXPECT_EQ(h->r32(kDesc + 0), 32u);
    EXPECT_EQ(h->r32(kDesc + 4), 16u);
    EXPECT_EQ(h->r32(kDesc + 8), static_cast<u32>(DXGI_FORMAT_R8G8B8A8_UNORM));
    EXPECT_EQ(h->r32(kDesc + 28), 2u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {0}, {0}, {0}, {0}}),
              S_OK);
    ASSERT_EQ(h->com(chain, SwapChainSlot::GetDesc1, {{kDesc}}), S_OK);
    EXPECT_EQ(h->r32(kDesc + 0), 120u);
    EXPECT_EQ(h->r32(kDesc + 4), 48u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{1}, {32}, {16}, {0}, {0}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {32}, {16}, {0}, {0x800 /* ALLOW_TEARING */}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::AddRef, {}), queue_refs);
    (void)h->com(queue, CommandQueueSlot::Release, {});
}

}  // namespace
}  // namespace vitreo::winapi
