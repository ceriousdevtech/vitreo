/// @file test_inproc_servers.cpp
/// @brief quartz / devenum / d3dcompiler / wbemprox / mmdevapi LoadLibrary exports fail honestly.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/media/inproc_servers/inproc_servers.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

class InprocServersTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(MEM_BASE, 0x10000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(STACK_BASE, 0x1000,
                                       MemoryProtection::ReadWrite).is_ok());
        cpu_.init_protected_mode();
        registry_ = std::make_unique<ThunkRegistry>(0x70000000);
        ASSERT_TRUE(registry_->register_dll("quartz.dll", create_quartz_thunks()).is_ok());
        ASSERT_TRUE(registry_->register_dll("devenum.dll", create_devenum_thunks()).is_ok());
        ASSERT_TRUE(registry_->register_dll(
            "d3dcompiler_47.dll", create_d3dcompiler_47_thunks()).is_ok());
        ASSERT_TRUE(registry_->register_dll("wbemprox.dll", create_wbemprox_thunks()).is_ok());
        ASSERT_TRUE(registry_->register_dll("wbemdisp.dll", create_wbemdisp_thunks()).is_ok());
        ASSERT_TRUE(registry_->register_dll("mmdevapi.dll", create_mmdevapi_thunks()).is_ok());
    }

    void set_args(const std::vector<u32>& args) {
        const GuestAddr frame = STACK_BASE + 0x400;
        cpu_.set_sp(frame);
        (void)memory_.write_u32(frame, 0xDEADBEEF);
        for (usize i = 0; i < args.size(); ++i) {
            (void)memory_.write_u32(
                frame + 4 + static_cast<GuestAddr>(i * 4), args[i]);
        }
    }

    [[nodiscard]] Marshaler make_marshaler() {
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        return m;
    }

    [[nodiscard]] u32 return_value() const {
        return static_cast<u32>(cpu_.gpr.rax & 0xFFFFFFFFu);
    }

    static constexpr GuestAddr MEM_BASE = 0x10000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr BUF_ADDR = MEM_BASE + 0x200;
    static constexpr GuestAddr PPV_ADDR = MEM_BASE + 0x100;

    GuestMemory memory_;
    cpu::CpuState cpu_;
    std::unique_ptr<ThunkRegistry> registry_;
};

TEST_F(InprocServersTest, QuartzDllGetClassObjectUnavailable) {
    (void)memory_.write_u32(PPV_ADDR, 0xFFFFFFFFu);
    set_args({static_cast<u32>(MEM_BASE), static_cast<u32>(MEM_BASE + 16),
              static_cast<u32>(PPV_ADDR)});
    auto m = make_marshaler();
    auto addr = registry_->get_thunk_address("quartz.dll", "DllGetClassObject");
    ASSERT_TRUE(addr.is_ok());
    auto handler = registry_->resolve_handler(addr.value());
    ASSERT_TRUE(handler.is_ok());
    (*handler.value())(m);
    EXPECT_EQ(return_value(), 0x80040111u);
    auto ppv = memory_.read_u32(PPV_ADDR);
    ASSERT_TRUE(ppv.is_ok());
    EXPECT_EQ(ppv.value(), 0u);
}

TEST_F(InprocServersTest, AmGetErrorTextAForNotImpl) {
    set_args({0x80004001u, static_cast<u32>(BUF_ADDR), 64});
    auto m = make_marshaler();
    auto addr = registry_->get_thunk_address("quartz.dll", "AMGetErrorTextA");
    ASSERT_TRUE(addr.is_ok());
    auto handler = registry_->resolve_handler(addr.value());
    ASSERT_TRUE(handler.is_ok());
    (*handler.value())(m);
    EXPECT_GT(return_value(), 0u);
    char buf[64]{};
    ASSERT_TRUE(memory_.read_bytes(BUF_ADDR, reinterpret_cast<u8*>(buf), 32).is_ok());
    EXPECT_STREQ(buf, "Not implemented.");
}

TEST_F(InprocServersTest, AmGetErrorTextAUnknownReturnsZero) {
    set_args({0x8000DEADu, static_cast<u32>(BUF_ADDR), 64});
    auto m = make_marshaler();
    auto addr = registry_->get_thunk_address("quartz.dll", "AMGetErrorTextA");
    ASSERT_TRUE(addr.is_ok());
    auto handler = registry_->resolve_handler(addr.value());
    ASSERT_TRUE(handler.is_ok());
    (*handler.value())(m);
    EXPECT_EQ(return_value(), 0u);
}

TEST_F(InprocServersTest, D3DCompileReturnsENotImpl) {
    set_args({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    auto m = make_marshaler();
    auto addr = registry_->get_thunk_address("d3dcompiler_47.dll", "D3DCompile");
    ASSERT_TRUE(addr.is_ok());
    auto handler = registry_->resolve_handler(addr.value());
    ASSERT_TRUE(handler.is_ok());
    (*handler.value())(m);
    EXPECT_EQ(return_value(), 0x80004001u);
}

TEST_F(InprocServersTest, DevenumIsLoadable) {
    auto addr = registry_->get_thunk_address("devenum.dll", "DllCanUnloadNow");
    ASSERT_TRUE(addr.is_ok());
    EXPECT_NE(addr.value(), 0u);
}

TEST_F(InprocServersTest, WbemproxDllGetClassObjectUnavailable) {
    (void)memory_.write_u32(PPV_ADDR, 0xFFFFFFFFu);
    set_args({static_cast<u32>(MEM_BASE), static_cast<u32>(MEM_BASE + 16),
              static_cast<u32>(PPV_ADDR)});
    auto m = make_marshaler();
    auto addr = registry_->get_thunk_address("wbemprox.dll", "DllGetClassObject");
    ASSERT_TRUE(addr.is_ok());
    auto handler = registry_->resolve_handler(addr.value());
    ASSERT_TRUE(handler.is_ok());
    (*handler.value())(m);
    EXPECT_EQ(return_value(), 0x80040111u);
    auto ppv = memory_.read_u32(PPV_ADDR);
    ASSERT_TRUE(ppv.is_ok());
    EXPECT_EQ(ppv.value(), 0u);
}

TEST_F(InprocServersTest, MmdevapiIsLoadable) {
    auto addr = registry_->get_thunk_address("mmdevapi.dll", "DllCanUnloadNow");
    ASSERT_TRUE(addr.is_ok());
    EXPECT_NE(addr.value(), 0u);
}

TEST_F(InprocServersTest, AcmUnavailableConversionNeverReturnsAStream) {
    const auto exports = create_msacm32_thunks();
    for (const auto& entry : exports.entries) {
        if (entry.name == "acmStreamOpen") {
            set_args({PPV_ADDR, 0, BUF_ADDR, BUF_ADDR, 0, 0, 0, 0});
            ASSERT_TRUE(memory_.write_u64(PPV_ADDR, ~u64{0}).is_ok());
            auto m = make_marshaler();
            entry.handler(m);
            EXPECT_EQ(return_value(), 512u);
            EXPECT_EQ(memory_.read_u64(PPV_ADDR).value(), 0xffffffff00000000ull);
            set_args({0, 0, BUF_ADDR, BUF_ADDR, 0, 0, 0, 1});
            entry.handler(m);
            EXPECT_EQ(return_value(), 512u);
            cpu_.init_long_mode();
            cpu_.set_sp(STACK_BASE + 0x400);
            cpu_.gpr.rcx = PPV_ADDR;
            cpu_.gpr.rdx = 0;
            cpu_.gpr.r8 = BUF_ADDR;
            cpu_.gpr.r9 = BUF_ADDR;
            for (u32 i=0; i<4; ++i) (void)memory_.write_u64(cpu_.sp()+0x28+i*8, 0);
            ASSERT_TRUE(memory_.write_u64(PPV_ADDR, ~u64{0}).is_ok());
            entry.handler(m);
            EXPECT_EQ(return_value(), 512u);
            EXPECT_EQ(memory_.read_u64(PPV_ADDR).value(), 0u);
        } else if (entry.name.starts_with("acmStream")) {
            auto m = make_marshaler();
            entry.handler(m);
            EXPECT_EQ(return_value(), 5u);
        }
    }
}

// ---- msdmo Mo*MediaType -----------------------------------------------------
// Expected values are the Windows 11 measurements in
// tools/oracle_probe/gfx_media_exports_probe(.x86).expected.
namespace {

struct BumpTaskMemory {
    GuestAddr next = 0x14000;
    std::vector<GuestAddr> freed;
    TaskMemoryFns fns() {
        return {[this](u64 n) { const GuestAddr p = next; next += (n + 15) & ~u64{15}; return p; },
                [this](GuestAddr p) { freed.push_back(p); }};
    }
};

const ExportEntry* msdmo_export(const DllExports& e, const char* name) {
    for (const auto& x : e.entries) if (x.name == name) return &x;
    return nullptr;
}

} // namespace

TEST_F(InprocServersTest, MsdmoMediaTypeHelpersMatchWindowsX86) {
    BumpTaskMemory task;
    const DllExports e = create_msdmo_thunks(task.fns());
    auto call = [&](const char* name, std::vector<u32> args) {
        const ExportEntry* x = msdmo_export(e, name);
        EXPECT_NE(x, nullptr) << name;
        if (x == nullptr) return 0xFFFFFFFFu;
        EXPECT_EQ(x->stdcall_bytes, args.size() * 4) << name;
        set_args(args);
        auto m = make_marshaler();
        x->handler(m);
        return return_value();
    };
    constexpr u32 kEPointer = 0x80004003u;
    const GuestAddr mt = MEM_BASE + 0x400;   // x86 DMO_MEDIA_TYPE: 72 bytes
    for (u32 i = 0; i < 72; i += 4) (void)memory_.write_u32(mt + i, 0xCCCCCCCCu);

    EXPECT_EQ(call("MoInitMediaType", {static_cast<u32>(mt), 16}), 0u);
    EXPECT_EQ(memory_.read_u32(mt + 56).value(), 0xCCCCCCCCu);   // header untouched
    EXPECT_EQ(memory_.read_u32(mt + 60).value(), 0u);            // pUnk
    EXPECT_EQ(memory_.read_u32(mt + 64).value(), 16u);           // cbFormat
    const u32 block = memory_.read_u32(mt + 68).value();
    EXPECT_NE(block, 0u);
    EXPECT_EQ(call("MoFreeMediaType", {static_cast<u32>(mt)}), 0u);
    EXPECT_EQ(memory_.read_u32(mt + 68).value(), 0u);
    EXPECT_EQ(memory_.read_u32(mt + 64).value(), 16u);           // cbFormat kept
    ASSERT_EQ(task.freed.size(), 1u);
    EXPECT_EQ(task.freed[0], block);
    EXPECT_EQ(call("MoFreeMediaType", {static_cast<u32>(mt)}), 0u);  // idempotent
    EXPECT_EQ(call("MoInitMediaType", {static_cast<u32>(mt), 0}), 0u);
    EXPECT_EQ(memory_.read_u32(mt + 68).value(), 0u);            // no block for 0
    EXPECT_EQ(call("MoInitMediaType", {0, 4}), kEPointer);
    EXPECT_EQ(call("MoFreeMediaType", {0}), kEPointer);

    // Copy: every field except pUnk, into a fresh format block; no AddRef.
    EXPECT_EQ(call("MoInitMediaType", {static_cast<u32>(mt), 4}), 0u);
    (void)memory_.write_u32(mt, 0x11111111u);
    (void)memory_.write_u32(mt + 40, 4096u);
    (void)memory_.write_u32(mt + 60, 0x55550000u);                // pUnk (never called)
    (void)memory_.write_u32(memory_.read_u32(mt + 68).value(), 0x04030201u);
    const GuestAddr dst = MEM_BASE + 0x500;
    EXPECT_EQ(call("MoCopyMediaType", {static_cast<u32>(dst), static_cast<u32>(mt)}), 0u);
    EXPECT_EQ(memory_.read_u32(dst).value(), 0x11111111u);
    EXPECT_EQ(memory_.read_u32(dst + 40).value(), 4096u);
    EXPECT_EQ(memory_.read_u32(dst + 60).value(), 0u);
    EXPECT_EQ(memory_.read_u32(dst + 64).value(), 4u);
    const u32 dst_block = memory_.read_u32(dst + 68).value();
    EXPECT_NE(dst_block, memory_.read_u32(mt + 68).value());
    EXPECT_EQ(memory_.read_u32(dst_block).value(), 0x04030201u);
    EXPECT_EQ(call("MoCopyMediaType", {0, static_cast<u32>(mt)}), kEPointer);
    EXPECT_EQ(call("MoCopyMediaType", {static_cast<u32>(dst), 0}), kEPointer);

    // Free with pUnk set NULLs it without Release (no guest call exists).
    EXPECT_EQ(call("MoFreeMediaType", {static_cast<u32>(mt)}), 0u);
    EXPECT_EQ(memory_.read_u32(mt + 60).value(), 0u);

    // Create / Duplicate / Delete: struct is task memory of sizeof 72.
    const GuestAddr pp = MEM_BASE + 0x600;
    const GuestAddr before = task.next;
    EXPECT_EQ(call("MoCreateMediaType", {static_cast<u32>(pp), 8}), 0u);
    const u32 created = memory_.read_u32(pp).value();
    EXPECT_EQ(created, before);
    EXPECT_EQ(memory_.read_u32(created + 64).value(), 8u);
    EXPECT_EQ(call("MoDeleteMediaType", {created}), 0u);
    EXPECT_EQ(task.freed.back(), created);
    EXPECT_EQ(call("MoCreateMediaType", {0, 8}), kEPointer);
    EXPECT_EQ(call("MoDeleteMediaType", {0}), kEPointer);
    (void)memory_.write_u32(pp, 1);
    EXPECT_EQ(call("MoDuplicateMediaType", {static_cast<u32>(pp), 0}), kEPointer);
    EXPECT_EQ(memory_.read_u32(pp).value(), 1u);                 // untouched
    EXPECT_EQ(call("MoDuplicateMediaType", {0, static_cast<u32>(dst)}), kEPointer);
    EXPECT_EQ(call("MoDuplicateMediaType", {static_cast<u32>(pp), static_cast<u32>(dst)}), 0u);
    const u32 dup = memory_.read_u32(pp).value();
    EXPECT_EQ(memory_.read_u32(dup).value(), 0x11111111u);
    EXPECT_EQ(memory_.read_u32(dup + 64).value(), 4u);
}

TEST_F(InprocServersTest, MsdmoMediaTypeUsesX64Layout) {
    BumpTaskMemory task;
    const DllExports e = create_msdmo_thunks(task.fns());
    const ExportEntry* init = msdmo_export(e, "MoInitMediaType");
    ASSERT_NE(init, nullptr);
    cpu_.init_long_mode();
    cpu_.set_sp(STACK_BASE + 0x400);
    const GuestAddr mt = MEM_BASE + 0x400;   // x64 DMO_MEDIA_TYPE: 88 bytes
    for (u32 i = 0; i < 88; i += 4) (void)memory_.write_u32(mt + i, 0xCCCCCCCCu);
    cpu_.gpr.rcx = mt;
    cpu_.gpr.rdx = 0xFFFFFFFF00000020ull;   // DWORD: only the low half is defined
    auto m = make_marshaler();
    init->handler(m);
    EXPECT_EQ(return_value(), 0u);
    EXPECT_EQ(memory_.read_u32(mt + 60).value(), 0xCCCCCCCCu);  // padding untouched
    EXPECT_EQ(memory_.read_u64(mt + 64).value(), 0u);           // pUnk
    EXPECT_EQ(memory_.read_u32(mt + 72).value(), 0x20u);        // cbFormat
    EXPECT_NE(memory_.read_u64(mt + 80).value(), 0u);           // pbFormat
}

TEST_F(InprocServersTest, AudioAsyncOrdinal17IsNamedActivation) {
    const auto by_name = registry_->get_thunk_address("mmdevapi.dll", "ActivateAudioInterfaceAsync");
    const auto by_ordinal = registry_->get_thunk_address("mmdevapi.dll", static_cast<u16>(17));
    ASSERT_TRUE(by_name.is_ok());
    ASSERT_TRUE(by_ordinal.is_ok());
    EXPECT_EQ(by_name.value(), by_ordinal.value());
}
