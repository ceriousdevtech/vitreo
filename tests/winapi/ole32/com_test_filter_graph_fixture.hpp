// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_test_filter_graph_fixture.hpp
/// @brief FilterGraphTest fixture (test_com_filter_graph_*.cpp and the base of
/// WasapiContractTest). Test-local.

#include "com_test_common.hpp"

class FilterGraphTest : public ::testing::Test {
protected:
    static constexpr GuestAddr DATA_BASE = 0x00100000;
    static constexpr GuestAddr STACK_BASE = 0x00200000;
    static constexpr GuestAddr COM_BASE = ComCoreManager::COM_SCRATCH_BASE;
    static constexpr GuestAddr CLSID_ADDR = DATA_BASE;
    static constexpr GuestAddr IID_ADDR = DATA_BASE + 0x20;
    static constexpr GuestAddr QI_IID_ADDR = DATA_BASE + 0x40;
    static constexpr GuestAddr OUT_ADDR = DATA_BASE + 0x80;
    static constexpr GuestAddr QI_OUT_ADDR = DATA_BASE + 0x84;
    static constexpr GuestAddr STATE_ADDR = DATA_BASE + 0x100;
    static constexpr GuestAddr VOLUME_ADDR = DATA_BASE + 0x104;

    static constexpr Guid CLSID_FILTERGRAPH{
        0xE436EBB3u, 0x524Fu, 0x11CEu,
        {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    static constexpr Guid IID_IGRAPHBUILDER{
        0x56A868A9u, 0x0AD4u, 0x11CEu,
        {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    static constexpr Guid IID_IMEDIACONTROL{
        0x56A868B1u, 0x0AD4u, 0x11CEu,
        {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    static constexpr Guid IID_IBASICAUDIO{
        0x56A868B3u, 0x0AD4u, 0x11CEu,
        {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    static constexpr Guid IID_IMEDIAEVENT{
        0x56A868B6u, 0x0AD4u, 0x11CEu,
        {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    static constexpr Guid IID_IMEDIAEVENTEX{
        0x56A868C0u, 0x0AD4u, 0x11CEu,
        {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    static constexpr Guid IID_IMEDIASEEKING{
        0x36B73880u, 0xC2C8u, 0x11CFu,
        {0x8B, 0x46, 0x00, 0x80, 0x5F, 0x6C, 0xEF, 0x60}};
    static constexpr Guid IID_IVIDEOWINDOW{
        0x56A868B4u, 0x0AD4u, 0x11CEu,
        {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    static constexpr Guid CLSID_FILTERGRAPH_NOTHREAD{
        0xE436EBB8u, 0x524Fu, 0x11CEu,
        {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    static constexpr Guid IID_IBASICVIDEO{
        0x56A868B5u, 0x0AD4u, 0x11CEu,
        {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    static constexpr Guid IID_IMEDIAPOSITION{
        0x56A868B2u, 0x0AD4u, 0x11CEu,
        {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    static constexpr Guid IID_IMEDIAEVENTSINK{
        0x56A868A2u, 0x0AD4u, 0x11CEu,
        {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};

    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(DATA_BASE, 0x10000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(STACK_BASE, 0x1000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(COM_BASE, 0x20000,
                                       MemoryProtection::ReadWrite).is_ok());
        cpu_.init_protected_mode();
        ASSERT_TRUE(
            registry_.register_dll("ole32.dll", create_com_core_thunks(mgr_)).is_ok());
        mgr_.set_thunk_registry(&registry_);
        mgr_.set_sync_manager(&sync_);
        // CoCreateInstance needs an apartment on the calling thread
        // (measured: CO_E_NOTINITIALIZED otherwise, s7_ole_probe.expected).
        (void)mgr_.co_initialize();
    }

    void set_args(const std::vector<u32>& args) {
        const GuestAddr frame = STACK_BASE + 0x400;
        cpu_.set_sp(frame);
        ASSERT_TRUE(memory_.write_u32(frame, 0xDEADBEEF).is_ok());
        for (usize index = 0; index < args.size(); ++index) {
            ASSERT_TRUE(memory_.write_u32(
                frame + 4 + static_cast<GuestAddr>(index * 4), args[index]).is_ok());
        }
    }

    [[nodiscard]] Marshaler make_marshaler() {
        Marshaler marshaler(memory_);
        marshaler.attach_cpu(cpu_);
        return marshaler;
    }

    void write_guid(GuestAddr address, const Guid& guid) {
        ASSERT_TRUE(memory_.write_bytes(
            address, reinterpret_cast<const Byte*>(&guid), sizeof(guid)).is_ok());
    }

    [[nodiscard]] u32 read_u32(GuestAddr address) {
        auto result = memory_.read_u32(address);
        EXPECT_TRUE(result.is_ok());
        return result.is_ok() ? result.value() : 0;
    }

    GuestMemory memory_;
    HandleTable handles_;
    SyncManager sync_{handles_};
    VirtualMemoryManager vmm_{memory_};
    HeapManager heap_manager_{vmm_, memory_, handles_};
    ComTaskMemoryManager task_memory_{heap_manager_};
    cpu::CpuState cpu_;
    ComCoreManager mgr_{task_memory_};
    ThunkRegistry registry_{0x70000000};
};

/// FilterGraphTest over both guest bitnesses (GetParam() == true is x64:
/// RCX/RDX/R8/R9 then stack slots after the 32-byte shadow area, doubles in
/// XMM; false is x86 stdcall on the stack). Fake guest IBaseFilter objects
/// live in DATA_BASE; their methods are FILTER_FN + slot * 0x10, answered
/// by the guest-call hook below.
class FilterGraphBitnessTest : public FilterGraphTest,
                               public ::testing::WithParamInterface<bool> {
protected:
    static constexpr GuestAddr FILTER_OBJS = DATA_BASE + 0x1000;  // 0x100 apart
    static constexpr GuestAddr FILTER_VTBL = DATA_BASE + 0x2000;
    static constexpr GuestAddr FILTER_FN = 0x00500000;
    static constexpr GuestAddr NAME_ADDR = DATA_BASE + 0x2400;
    static constexpr GuestAddr ARRAY_ADDR = DATA_BASE + 0x2800;
    static constexpr GuestAddr OUT2_ADDR = DATA_BASE + 0x2900;
    static constexpr GuestAddr OUT3_ADDR = DATA_BASE + 0x2910;

    struct FakeFilter {
        i32 refs{1};
        int joins{0};
        u64 graph{0};
        std::u16string name;
        u32 join_hr{0};
    };

    void SetUp() override {
        FilterGraphTest::SetUp();
        x64_ = GetParam();
        if (x64_) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        // 20 slots: enough for every interface the fakes stand in for
        // (IMediaSample has 19).
        for (u32 slot = 0; slot < 20; ++slot) {
            wptr(FILTER_VTBL + slot * ps(), FILTER_FN + slot * 0x10u);
        }
        for (u32 i = 0; i < 8; ++i) wptr(filter_obj(i), FILTER_VTBL);
        mgr_.set_guest_call([this](GuestAddr fn, const u64* args, usize count,
                                   bool is64) -> u64 {
            EXPECT_EQ(is64, x64_);
            EXPECT_GE(count, 1u);
            const u32 slot = static_cast<u32>((fn - FILTER_FN) / 0x10u);
            const u32 index =
                static_cast<u32>((static_cast<GuestAddr>(args[0]) - FILTER_OBJS) / 0x100u);
            if (fn < FILTER_FN || slot >= 20 || index >= 8) {
                ADD_FAILURE() << "unexpected guest call 0x" << std::hex << fn;
                return 0x80004005u;
            }
            FakeFilter& f = filters_[index];
            calls_.push_back({index, slot});
            if (slot == 1) return static_cast<u64>(++f.refs);
            if (slot == 2) return static_cast<u64>(--f.refs);
            if (slot == 13) {  // JoinFilterGraph(pGraph, pName)
                EXPECT_EQ(count, 3u);
                ++f.joins;
                f.graph = args[1];
                f.name = args[2] == 0 ? u"<null>" : wstr(static_cast<GuestAddr>(args[2]));
                return f.join_hr;
            }
            return 0;
        });
    }

    [[nodiscard]] GuestAddr ps() const { return x64_ ? 8u : 4u; }
    [[nodiscard]] static GuestAddr filter_obj(u32 index) {
        return FILTER_OBJS + index * 0x100u;
    }

    void wptr(GuestAddr address, u64 value) {
        if (x64_) ASSERT_TRUE(memory_.write_u64(address, value).is_ok());
        else ASSERT_TRUE(memory_.write_u32(address, static_cast<u32>(value)).is_ok());
    }
    [[nodiscard]] u64 ptr(GuestAddr address) {
        return x64_ ? memory_.read_u64(address).value_or(0xBAD)
                    : memory_.read_u32(address).value_or(0xBAD);
    }
    [[nodiscard]] std::u16string wstr(GuestAddr address) {
        std::u16string text;
        for (GuestAddr at = address;; at += 2) {
            const u16 unit = memory_.read_u16(at).value_or(0);
            if (unit == 0) break;
            text.push_back(static_cast<char16_t>(unit));
        }
        return text;
    }
    void put_wide(GuestAddr address, std::u16string_view text) {
        for (usize i = 0; i < text.size(); ++i) {
            ASSERT_TRUE(memory_.write_u16(address + static_cast<GuestAddr>(i * 2), text[i]).is_ok());
        }
        ASSERT_TRUE(memory_.write_u16(address + static_cast<GuestAddr>(text.size() * 2), 0).is_ok());
    }

    /// Lay out one call. x86: each value is one 4-byte stack slot (pass a
    /// 64-bit value as two). x64: one register/stack slot per value;
    /// `xmm_index` names an argument that travels in XMM (its GPR is junk).
    void put_args(std::initializer_list<u64> values, int xmm_index = -1) {
        const GuestAddr frame = STACK_BASE + 0x400;
        cpu_.set_sp(frame);
        int index = 0;
        for (const u64 value : values) {
            if (!x64_) {
                (void)memory_.write_u32(frame + 4 + 4 * static_cast<GuestAddr>(index),
                                        static_cast<u32>(value));
            } else if (index < 4) {
                const u64 gpr = index == xmm_index ? 0xBADF00D0BADF00D0ull : value;
                if (index == xmm_index) cpu_.sse.xmm[index].u64_[0] = value;
                if (index == 0) cpu_.gpr.rcx = gpr;
                if (index == 1) cpu_.gpr.rdx = gpr;
                if (index == 2) cpu_.gpr.r8 = gpr;
                if (index == 3) cpu_.gpr.r9 = gpr;
            } else {
                (void)memory_.write_u64(frame + 0x28 + 8 * static_cast<GuestAddr>(index - 4), value);
            }
            ++index;
        }
    }
    template <typename Fn>
    u32 invoke(Fn fn, std::initializer_list<u64> values) {
        put_args(values);
        auto m = make_marshaler();
        return static_cast<u32>((mgr_.*fn)(m));
    }
    /// Method `op` of a dispatcher taking (Marshaler&, u32).
    template <typename Fn>
    u32 invoke_op(Fn fn, u32 op, std::initializer_list<u64> values) {
        put_args(values);
        auto m = make_marshaler();
        return static_cast<u32>((mgr_.*fn)(m, op));
    }

    /// CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, iid, &out).
    GuestAddr create_graph(const Guid& iid = Guid{0x56A868A9u, 0x0AD4u, 0x11CEu,
                               {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}}) {
        write_guid(CLSID_ADDR, CLSID_FILTERGRAPH);
        write_guid(IID_ADDR, iid);
        // Pre-fill all 8 bytes: a 4-byte write would leave 0xCAFEBABE high.
        (void)memory_.write_u64(OUT_ADDR, 0xCAFEBABECAFEBABEull);
        put_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
        auto m = make_marshaler();
        EXPECT_EQ(mgr_.co_create_instance(m), com::S_OK);
        if (x64_) {
            EXPECT_EQ(memory_.read_u32(OUT_ADDR + 4).value_or(1), 0u);
        } else {
            EXPECT_EQ(memory_.read_u32(OUT_ADDR + 4).value_or(1), 0xCAFEBABEu);
        }
        return static_cast<GuestAddr>(ptr(OUT_ADDR));
    }
    GuestAddr qi(GuestAddr object, const Guid& iid) {
        write_guid(QI_IID_ADDR, iid);
        const GuestAddr out = DATA_BASE + 0x300;  // 8-aligned slot
        (void)memory_.write_u64(out, 0xCAFEBABECAFEBABEull);
        EXPECT_EQ(invoke(&ComCoreManager::filter_graph_query_interface,
                         {object, QI_IID_ADDR, out}),
                  com::S_OK);
        if (x64_) EXPECT_EQ(memory_.read_u32(out + 4).value_or(1), 0u);
        return static_cast<GuestAddr>(ptr(out));
    }
    [[nodiscard]] GuestAddr thunk(std::string_view name) {
        auto address = registry_.get_thunk_address("ole32.dll", name);
        EXPECT_TRUE(address.is_ok()) << name;
        return address.is_ok() ? address.value() : 0;
    }

    bool x64_{false};
    FakeFilter filters_[8];
    std::vector<std::pair<u32, u32>> calls_;  // (filter index, vtable slot)
};
