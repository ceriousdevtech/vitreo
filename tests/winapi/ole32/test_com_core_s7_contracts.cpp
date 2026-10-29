// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_core_s7_contracts.cpp
/// @brief S7 audit contracts measured by tools/oracle_probe/s7_ole_probe.expected.

#include "com_test_common.hpp"
#include "com_test_memory_stream_fixture.hpp"

// =============================================================================
// S7 audit: contracts measured by tools/oracle_probe/s7_ole_probe.expected
// (Windows 11, x86 and x64 identical).
// =============================================================================

namespace {
const ExportEntry* s7_export(const DllExports& exports, std::string_view name) {
    for (const auto& e : exports.entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}
} // namespace

TEST_F(MemoryStreamTest, S7StringFromIIDAllocatesBracedText) {
    auto exports = create_com_core_thunks(manager);
    const auto* entry = s7_export(exports, "StringFromIID");
    ASSERT_NE(entry, nullptr) << "missing, it bound to the 0xFFFFFFFF stub";
    EXPECT_EQ(entry->stdcall_bytes, 8u);
    constexpr GuestAddr iid = DATA_BASE + 0x40;
    constexpr GuestAddr out = DATA_BASE + 0x60;
    constexpr Guid kIDropTarget{0x00000122u, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid, kIDropTarget).is_ok());
    set_args({static_cast<u32>(iid), static_cast<u32>(out)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr text = memory.read_u32(out).value();
    EXPECT_EQ(marshaler.read_wide_string(text).value(),
              "{00000122-0000-0000-C000-000000000046}");
    EXPECT_TRUE(task_memory.owns(text));
}

TEST_F(MemoryStreamTest, S7ClsidFromStringParsesStrictlyWithPartialOutput) {
    auto exports = create_com_core_thunks(manager);
    const auto* entry = s7_export(exports, "CLSIDFromString");
    ASSERT_NE(entry, nullptr);
    constexpr GuestAddr text = DATA_BASE + 0x100;
    constexpr GuestAddr out = DATA_BASE + 0x200;
    const auto run = [&](const char* s) {
        std::string ascii(s);
        for (usize i = 0; i <= ascii.size(); ++i) {
            EXPECT_TRUE(memory.write_u16(text + i * 2,
                i < ascii.size() ? static_cast<u8>(ascii[i]) : 0).is_ok());
        }
        for (u32 i = 0; i < 16; ++i) EXPECT_TRUE(memory.write_u8(out + i, 0xCC).is_ok());
        set_args({static_cast<u32>(text), static_cast<u32>(out)});
        entry->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    EXPECT_EQ(run("{00000122-0000-0000-C000-000000000046}"), com::S_OK);
    EXPECT_EQ(memory.read_u32(out).value(), 0x122u);
    // bad#0: Data1 keeps the digits read before the bad one; the rest stays.
    EXPECT_EQ(run("{0000012Z-0000-0000-C000-000000000046}"), com::CO_E_CLASSSTRING);
    EXPECT_EQ(memory.read_u32(out).value(), 0x12u);
    EXPECT_EQ(memory.read_u32(out + 4).value(), 0xCCCCCCCCu);
    // bad#2: Data4[0] parsed, Data4[1] not.
    EXPECT_EQ(run("{00000122-0000-0000-C0Z0-000000000046}"), com::CO_E_CLASSSTRING);
    EXPECT_EQ(memory.read_u8(out + 8).value(), 0xC0u);
    EXPECT_EQ(memory.read_u8(out + 9).value(), 0xCCu);
    // Missing closing brace: every field written, still an error.
    EXPECT_EQ(run("{00000122-0000-0000-C000-000000000046"), com::CO_E_CLASSSTRING);
    EXPECT_EQ(memory.read_u8(out + 15).value(), 0x46u);
    // No brace: a ProgID lookup, which fails with the output zeroed.
    EXPECT_EQ(run("00000122-0000-0000-C000-000000000046"), com::CO_E_CLASSSTRING);
    EXPECT_EQ(memory.read_u32(out).value(), 0u);
    EXPECT_EQ(run(" {00000122-0000-0000-C000-000000000046}"), com::CO_E_CLASSSTRING);
    // NULL string: GUID_NULL and S_OK.
    for (u32 i = 0; i < 16; ++i) ASSERT_TRUE(memory.write_u8(out + i, 0xCC).is_ok());
    set_args({0u, static_cast<u32>(out)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(memory.read_u32(out).value(), 0u);
}

TEST_F(MemoryStreamTest, S7StringFromGUID2RejectsShortAndNegativeSizes) {
    auto exports = create_com_core_thunks(manager);
    const auto* entry = s7_export(exports, "StringFromGUID2");
    ASSERT_NE(entry, nullptr);
    constexpr GuestAddr guid = DATA_BASE + 0x40;
    constexpr GuestAddr buf = DATA_BASE + 0x100;
    ASSERT_TRUE(marshaler.write_struct<Guid>(guid, Guid{}).is_ok());
    for (const u32 cch : {39u, 38u, 0xFFFFFFFFu}) {
        set_args({static_cast<u32>(guid), static_cast<u32>(buf), cch});
        entry->handler(marshaler);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), cch == 39u ? 39u : 0u) << cch;
    }
}

TEST_F(MemoryStreamTest, S7PropVariantClearZeroesSixteenBytesOnX86AndValidatesVt) {
    auto exports = create_com_core_thunks(manager);
    const auto* entry = s7_export(exports, "PropVariantClear");
    ASSERT_NE(entry, nullptr);
    constexpr GuestAddr pv = DATA_BASE + 0x300;
    const auto reset = [&](u16 vt, u32 value) {
        for (u32 i = 0; i < 32; ++i) EXPECT_TRUE(memory.write_u8(pv + i, 0xAB).is_ok());
        EXPECT_TRUE(memory.write_u16(pv, vt).is_ok());
        EXPECT_TRUE(memory.write_u32(pv + 8, value).is_ok());
    };
    // VT_LPWSTR: CoTaskMem string freed; x86 PROPVARIANT is 16 bytes, and the
    // byte after it is untouched (the old code wrote 24).
    const GuestAddr str = manager.co_task_mem_alloc_guest(8);
    reset(31, static_cast<u32>(str));
    set_args({static_cast<u32>(pv)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(memory.read_u64(pv).value(), 0u);
    EXPECT_EQ(memory.read_u64(pv + 8).value(), 0u);
    EXPECT_EQ(memory.read_u8(pv + 16).value(), 0xABu);
    EXPECT_FALSE(task_memory.owns(str));
    // Invalid vt: STG_E_INVALIDPARAMETER, structure still zeroed.
    reset(0x7F, 5);
    set_args({static_cast<u32>(pv)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80030057u);
    EXPECT_EQ(memory.read_u16(pv).value(), 0u);
    // By-reference: nothing freed, S_OK. NULL: S_OK.
    reset(3 | 0x4000, static_cast<u32>(DATA_BASE));
    set_args({static_cast<u32>(pv)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    set_args({0u});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
}

TEST_F(MemoryStreamTest, S7PropVariantCopyDuplicatesStringsAndRejectsBadVt) {
    auto exports = create_com_core_thunks(manager);
    const auto* entry = s7_export(exports, "PropVariantCopy");
    ASSERT_NE(entry, nullptr);
    constexpr GuestAddr dst = DATA_BASE + 0x400;
    constexpr GuestAddr src = DATA_BASE + 0x420;
    const GuestAddr str = manager.co_task_mem_alloc_guest(8);
    ASSERT_TRUE(marshaler.write_wide_string(str, "abc").is_ok());
    ASSERT_TRUE(memory.write_u16(src, 31).is_ok());       // VT_LPWSTR
    ASSERT_TRUE(memory.write_u32(src + 8, static_cast<u32>(str)).is_ok());
    set_args({static_cast<u32>(dst), static_cast<u32>(src)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr copy = memory.read_u32(dst + 8).value();
    EXPECT_NE(copy, str);
    EXPECT_EQ(marshaler.read_wide_string(copy).value(), "abc");
    EXPECT_TRUE(task_memory.owns(copy));
    // Invalid vt: DISP_E_BADVARTYPE with the destination untouched.
    for (u32 i = 0; i < 16; ++i) ASSERT_TRUE(memory.write_u8(dst + i, 0xAB).is_ok());
    ASSERT_TRUE(memory.write_u16(src, 0x7F).is_ok());
    set_args({static_cast<u32>(dst), static_cast<u32>(src)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80020008u);
    EXPECT_EQ(memory.read_u32(dst).value(), 0xABABABABu);
}

TEST_F(MemoryStreamTest, S7CoCreateInstanceNeedsAnApartmentAndReportsUnregistered) {
    ComCoreManager fresh{task_memory};
    ThunkRegistry fresh_registry{0x71000000};
    ASSERT_TRUE(fresh_registry.register_dll("ole32.dll", create_com_core_thunks(fresh)).is_ok());
    fresh.set_thunk_registry(&fresh_registry);
    auto exports = create_com_core_thunks(fresh);
    const auto* create = s7_export(exports, "CoCreateInstance");
    ASSERT_NE(create, nullptr);
    constexpr GuestAddr clsid = DATA_BASE + 0x40;
    constexpr GuestAddr iid = DATA_BASE + 0x60;
    constexpr GuestAddr out = DATA_BASE + 0x80;
    constexpr Guid kUnregistered{0x5E7C0000u, 0x1234u, 0x4ABCu,
                                 {0x80, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}};
    constexpr Guid kIUnknown{0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid, kUnregistered).is_ok());
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid, kIUnknown).is_ok());
    ASSERT_TRUE(memory.write_u32(out, 0x1111u).is_ok());
    set_args({static_cast<u32>(clsid), 0u, 1u, static_cast<u32>(iid), static_cast<u32>(out)});
    create->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x800401F0u);  // CO_E_NOTINITIALIZED
    EXPECT_EQ(memory.read_u32(out).value(), 0u);

    (void)fresh.co_initialize();
    ASSERT_TRUE(memory.write_u32(out, 0x1111u).is_ok());
    set_args({static_cast<u32>(clsid), 0u, 1u, static_cast<u32>(iid), static_cast<u32>(out)});
    create->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::REGDB_E_CLASSNOTREG);
    EXPECT_EQ(memory.read_u32(out).value(), 0u);
    set_args({static_cast<u32>(clsid), 0u, 1u, static_cast<u32>(iid), 0u});
    create->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::E_INVALIDARG);  // NULL ppv
}

TEST_F(MemoryStreamTest, S7IIDFromStringRejectsBadHexWithIidString) {
    auto exports = create_com_core_thunks(manager);
    const auto* entry = s7_export(exports, "IIDFromString");
    ASSERT_NE(entry, nullptr);
    constexpr GuestAddr text = DATA_BASE + 0x100;
    constexpr GuestAddr out = DATA_BASE + 0x200;
    ASSERT_TRUE(marshaler.write_wide_string(
        text, "{0000012Z-0000-0000-C000-000000000046}").is_ok());
    set_args({static_cast<u32>(text), static_cast<u32>(out)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x800401F4u);  // CO_E_IIDSTRING
}

// S7 audit: CreateStreamOnHGlobal ignored hGlobal and GetHGlobalFromStream
// always failed; ReleaseStgMedium did nothing; RegisterDragDrop faked S_OK.
// Expected values: tools/oracle_probe/s7_ole_probe.expected.
TEST_F(MemoryStreamTest, S7HGlobalStreamsReleaseStgMediumAndDragDropGate) {
    // The streams' backings free their HGLOBAL when the fixture's manager is
    // destroyed, after this body returns, so the allocator must outlive it
    // (heap_manager is destroyed after manager).
    auto& globals = *new GlobalLocalManager{heap_manager};
    manager.set_global_local_manager(&globals);
    auto exports = create_com_core_thunks(manager);
    const auto call = [&](std::string_view name, std::initializer_list<u32> args) {
        const auto* entry = s7_export(exports, name);
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 0xBADu;
        EXPECT_EQ(entry->stdcall_bytes, args.size() * 4) << name;
        set_args(args);
        entry->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    constexpr GuestAddr out = DATA_BASE + 0x40;
    constexpr GuestAddr out2 = DATA_BASE + 0x48;

    const u64 hglobal = globals.global_alloc(0x2 /*GMEM_MOVEABLE*/, 5);
    ASSERT_NE(hglobal, 0u);
    EXPECT_EQ(call("CreateStreamOnHGlobal", {static_cast<u32>(hglobal), 0u, 0u}),
              com::E_INVALIDARG);  // NULL ppstm
    ASSERT_EQ(call("CreateStreamOnHGlobal", {static_cast<u32>(hglobal), 0u, out}),
              com::S_OK);
    const u32 stream = memory.read_u32(out).value();
    ASSERT_NE(stream, 0u);
    EXPECT_EQ(call("GetHGlobalFromStream", {stream, out2}), com::S_OK);
    EXPECT_EQ(memory.read_u32(out2).value(), static_cast<u32>(hglobal));
    EXPECT_EQ(call("GetHGlobalFromStream", {0u, out2}), com::E_INVALIDARG);

    ASSERT_EQ(call("CreateStreamOnHGlobal", {0u, 1u, out}), com::S_OK);
    ASSERT_EQ(call("GetHGlobalFromStream", {memory.read_u32(out).value(), out2}), com::S_OK);
    EXPECT_NE(memory.read_u32(out2).value(), 0u);  // a new block

    // ReleaseStgMedium: TYMED_HGLOBAL with no pUnkForRelease frees the block
    // and leaves tymed TYMED_NULL. x86 STGMEDIUM: tymed @0, hGlobal @4, pUnk @8.
    const u64 owned = globals.global_alloc(0x2, 16);
    constexpr GuestAddr medium = DATA_BASE + 0x80;
    ASSERT_TRUE(memory.write_u32(medium, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(medium + 4, static_cast<u32>(owned)).is_ok());
    ASSERT_TRUE(memory.write_u32(medium + 8, 0).is_ok());
    (void)call("ReleaseStgMedium", {medium});
    EXPECT_EQ(memory.read_u32(medium).value(), 0u);
    EXPECT_EQ(globals.global_size(owned), 0u);

    // RegisterDragDrop needs OleInitialize (CoInitialize alone: E_OUTOFMEMORY);
    // after it, a NULL hwnd is DRAGDROP_E_INVALIDHWND.
    EXPECT_EQ(call("RegisterDragDrop", {0x1234u, DATA_BASE + 0x100}), 0x8007000Eu);
    EXPECT_EQ(call("OleInitialize", {0u}), com::S_OK);
    EXPECT_EQ(call("RegisterDragDrop", {0u, DATA_BASE + 0x100}), 0x80040102u);
    EXPECT_EQ(call("RevokeDragDrop", {0u}), 0x80040102u);
}

// S7 audit: RoGetApartmentIdentifier / RoUnregisterForApartmentShutdown were
// missing (fake-success stubs). s7_winrt_probe.expected: no apartment ->
// CO_E_NOTINITIALIZED with *id = 0; an apartment has a non-zero id; an
// unknown cookie is E_INVALIDARG.
TEST_F(MemoryStreamTest, S7WinrtApartmentIdentifierFollowsTheApartment) {
    ComCoreManager fresh{task_memory};
    constexpr GuestAddr id = DATA_BASE + 0x40;
    ASSERT_TRUE(memory.write_u64(id, 0x5555).is_ok());
    set_args({});
    EXPECT_EQ(fresh.ro_get_apartment_identifier(marshaler, id), 0x800401F0u);
    EXPECT_EQ(memory.read_u64(id).value(), 0u);
    (void)fresh.co_initialize();
    EXPECT_EQ(fresh.ro_get_apartment_identifier(marshaler, id), com::S_OK);
    EXPECT_NE(memory.read_u64(id).value(), 0u);
    EXPECT_EQ(fresh.ro_unregister_for_apartment_shutdown(marshaler, 0x1234), com::E_INVALIDARG);
    EXPECT_EQ(fresh.ro_unregister_for_apartment_shutdown(marshaler, 0), com::E_INVALIDARG);
}
