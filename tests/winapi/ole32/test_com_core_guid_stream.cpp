// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_core_guid_stream.cpp
/// @brief CoCreateGuid, call context, IIDFromString and OleSave/OleLoadFromStream tests.

#include "com_test_common.hpp"
#include "com_test_memory_stream_fixture.hpp"

TEST_F(MemoryStreamTest, CoCreateGuidWritesUuidAndRejectsNull) {
    auto exports = create_com_core_thunks(manager);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "CoCreateGuid";
        });
    ASSERT_NE(entry, exports.entries.end());

    constexpr GuestAddr output = DATA_BASE + 0x80;
    const std::array<u8, 16> poison = {
        0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD,
        0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD,
    };
    ASSERT_TRUE(
        memory.write_bytes(output, poison.data(), poison.size()).is_ok());
    set_args({static_cast<u32>(output)});
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    std::array<u8, 16> uuid{};
    ASSERT_TRUE(memory.read_bytes(output, uuid.data(), uuid.size()).is_ok());
    EXPECT_NE(uuid, poison);
    // Version nibble is in octet 7, not 6: these bytes are a GUID *struct*
    // (Data1/Data2/Data3 little-endian), so RFC 4122's wire octet 6 -- the
    // high byte of Data3 -- sits at struct offset 7. Verified against real
    // Windows, whose UuidCreate yields octet6=0xbb, octet7=0x4c.
    EXPECT_EQ(uuid[7] & 0xF0, 0x40);
    // Data4 is not byte-swapped, so the variant stays at octet 8.
    EXPECT_EQ(uuid[8] & 0xC0, 0x80);

    set_args({0});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::E_INVALIDARG);
}

TEST_F(MemoryStreamTest, CoGetCallContextRejectsUnsupportedInterfaceCleanly) {
    auto exports = create_com_core_thunks(manager);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "CoGetCallContext";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 8u);

    // Chromium's optional ICallingProcessInfo interface. A COM runtime that
    // does not expose this interface must return E_NOINTERFACE, not an invalid
    // success-like fallback value.
    constexpr GuestAddr iid = DATA_BASE + 0x90;
    constexpr GuestAddr output = DATA_BASE + 0xA0;
    constexpr Guid calling_process_info{
        0x68C6A1B9u, 0xDE39u, 0x42C3u,
        {0x8D, 0x28, 0xBF, 0x40, 0xA5, 0x12, 0x65, 0x41}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid, calling_process_info).is_ok());
    ASSERT_TRUE(memory.write_u32(output, 0xCCCCCCCCu).is_ok());
    set_args({static_cast<u32>(iid), static_cast<u32>(output)});
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::E_NOINTERFACE);
    EXPECT_EQ(memory.read_u32(output).value(), 0u);

    set_args({static_cast<u32>(iid), 0});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::E_POINTER);
}

TEST_F(MemoryStreamTest,
       IncomingCallContextExposesServerSecurityAndImpersonatesCaller) {
    auto exports = create_com_core_thunks(manager);
    const auto find_export = [&](const char* name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
    };
    auto get_context = find_export("CoGetCallContext");
    auto impersonate = find_export("Vitreo_ServerSecurity_ImpersonateClient");
    auto is_impersonating =
        find_export("Vitreo_ServerSecurity_IsImpersonating");
    auto revert = find_export("Vitreo_ServerSecurity_RevertToSelf");
    auto release = find_export("Vitreo_ServerSecurity_Release");
    ASSERT_NE(get_context, exports.entries.end());
    ASSERT_NE(impersonate, exports.entries.end());
    ASSERT_NE(is_impersonating, exports.entries.end());
    ASSERT_NE(revert, exports.entries.end());
    ASSERT_NE(release, exports.entries.end());

    constexpr GuestAddr iid = DATA_BASE + 0xB0;
    constexpr GuestAddr output = DATA_BASE + 0xC0;
    constexpr Guid server_security{
        0x0000013Eu, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid, server_security).is_ok());

    bool observed_elevated = false;
    u32 revert_count = 0;
    manager.set_caller_impersonation(
        [&](bool elevated) {
            observed_elevated = elevated;
            return true;
        },
        [&] { ++revert_count; });
    const auto previous = manager.push_server_call_context(true);

    set_args({static_cast<u32>(iid), static_cast<u32>(output)});
    get_context->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const u32 object = memory.read_u32(output).value();
    ASSERT_NE(object, 0u);

    set_args({object});
    impersonate->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_TRUE(observed_elevated);

    set_args({object});
    is_impersonating->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    set_args({object});
    revert->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(revert_count, 1u);

    set_args({object});
    release->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    manager.pop_server_call_context(previous);
    EXPECT_EQ(revert_count, 1u);
}

TEST_F(MemoryStreamTest, IIDFromStringParsesBracedGuid) {
    auto exports = create_com_core_thunks(manager);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "IIDFromString";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 8u);

    constexpr GuestAddr str_addr = DATA_BASE + 0x100;
    constexpr GuestAddr iid_addr = DATA_BASE + 0x200;
    // {6B29FC40-CA47-1067-B31D-00DD010662DA}
    const char16_t braced[] =
        u"{6B29FC40-CA47-1067-B31D-00DD010662DA}";
    for (usize i = 0; i < std::size(braced); ++i) {
        ASSERT_TRUE(memory.write_u16(
            str_addr + static_cast<u64>(i) * 2,
            static_cast<u16>(braced[i])).is_ok());
    }

    set_args({static_cast<u32>(str_addr), static_cast<u32>(iid_addr)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);

    Guid g{};
    ASSERT_TRUE(memory.read_bytes(iid_addr, reinterpret_cast<u8*>(&g),
                                  sizeof(g)).is_ok());
    EXPECT_EQ(g.Data1, 0x6B29FC40u);
    EXPECT_EQ(g.Data2, 0xCA47u);
    EXPECT_EQ(g.Data3, 0x1067u);
    EXPECT_EQ(g.Data4[0], 0xB3u);
    EXPECT_EQ(g.Data4[1], 0x1Du);
}

TEST_F(MemoryStreamTest, OleSaveToStreamWritesClsidThenSavesAndClearsDirty) {
    constexpr GuestAddr persist = DATA_BASE + 0x100;
    constexpr GuestAddr persist_vtable = DATA_BASE + 0x200;
    constexpr GuestAddr stream = DATA_BASE + 0x300;
    constexpr GuestAddr stream_vtable = DATA_BASE + 0x400;
    constexpr GuestAddr get_class_id = 0x00401000;
    constexpr GuestAddr write = 0x00402000;
    constexpr GuestAddr save = 0x00403000;
    constexpr std::array<u8, 16> clsid{
        0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56,
        0x90, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67};

    ASSERT_TRUE(memory.write_u32(persist, persist_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(stream, stream_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(
        persist_vtable + 3 * 4, get_class_id).is_ok());
    ASSERT_TRUE(memory.write_u32(
        persist_vtable + 6 * 4, save).is_ok());
    ASSERT_TRUE(memory.write_u32(stream_vtable + 4 * 4, write).is_ok());

    std::vector<GuestAddr> calls;
    manager.set_guest_call(
        [&](GuestAddr method, const u64* args, usize count, bool is_64bit) {
            EXPECT_FALSE(is_64bit);
            calls.push_back(method);
            if (method == get_class_id) {
                EXPECT_EQ(count, 2u);
                EXPECT_EQ(args[0], persist);
                EXPECT_TRUE(memory.write_bytes(
                    args[1], clsid.data(), clsid.size()).is_ok());
            } else if (method == write) {
                EXPECT_EQ(count, 4u);
                EXPECT_EQ(args[0], stream);
                EXPECT_EQ(args[2], clsid.size());
                EXPECT_EQ(args[3], 0u);
                std::array<u8, 16> observed{};
                EXPECT_TRUE(memory.read_bytes(
                    args[1], observed.data(), observed.size()).is_ok());
                EXPECT_EQ(observed, clsid);
            } else if (method == save) {
                EXPECT_EQ(count, 3u);
                EXPECT_EQ(args[0], persist);
                EXPECT_EQ(args[1], stream);
                EXPECT_EQ(args[2], 1u);
            }
            return static_cast<u64>(com::S_OK);
        });

    auto exports = create_com_core_thunks(manager);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "OleSaveToStream";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 8u);

    set_args({static_cast<u32>(persist), static_cast<u32>(stream)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(calls, (std::vector<GuestAddr>{get_class_id, write, save}));
}

TEST_F(MemoryStreamTest, OleLoadFromStreamActivatesLoadsQueriesAndReleases) {
    constexpr GuestAddr stream = DATA_BASE + 0x100;
    constexpr GuestAddr stream_vtable = DATA_BASE + 0x200;
    constexpr GuestAddr persist = DATA_BASE + 0x300;
    constexpr GuestAddr persist_vtable = DATA_BASE + 0x400;
    constexpr GuestAddr requested_iid = DATA_BASE + 0x500;
    constexpr GuestAddr out_ptr = DATA_BASE + 0x520;
    constexpr GuestAddr requested_object = DATA_BASE + 0x600;
    constexpr GuestAddr read = 0x00411000;
    constexpr GuestAddr query_interface = 0x00412000;
    constexpr GuestAddr release = 0x00413000;
    constexpr GuestAddr load = 0x00414000;
    constexpr std::array<u8, 16> clsid{
        0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56,
        0x90, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67};
    constexpr std::array<u8, 16> iid{
        0xEF, 0xBE, 0xAD, 0xDE, 0x11, 0x11, 0x22, 0x22,
        0x80, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    ASSERT_TRUE(memory.write_u32(stream, stream_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(stream_vtable + 3 * 4, read).is_ok());
    ASSERT_TRUE(memory.write_u32(persist, persist_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(
        persist_vtable + 0 * 4, query_interface).is_ok());
    ASSERT_TRUE(memory.write_u32(
        persist_vtable + 2 * 4, release).is_ok());
    ASSERT_TRUE(memory.write_u32(persist_vtable + 5 * 4, load).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        requested_iid, iid.data(), iid.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(out_ptr, 0xCCCCCCCCu).is_ok());

    const auto co_create = registry.get_thunk_address(
        "ole32.dll", "CoCreateInstance");
    ASSERT_TRUE(co_create.is_ok());
    std::vector<GuestAddr> calls;
    manager.set_guest_call(
        [&](GuestAddr method, const u64* args, usize count, bool is_64bit) {
            EXPECT_FALSE(is_64bit);
            calls.push_back(method);
            if (method == read) {
                EXPECT_EQ(count, 4u);
                EXPECT_EQ(args[0], stream);
                EXPECT_EQ(args[2], clsid.size());
                EXPECT_TRUE(memory.write_bytes(
                    args[1], clsid.data(), clsid.size()).is_ok());
            } else if (method == co_create.value()) {
                EXPECT_EQ(count, 5u);
                EXPECT_EQ(args[1], 0u);
                EXPECT_EQ(args[2], 1u);
                EXPECT_TRUE(memory.write_u32(args[4], persist).is_ok());
            } else if (method == load) {
                EXPECT_EQ(count, 2u);
                EXPECT_EQ(args[0], persist);
                EXPECT_EQ(args[1], stream);
            } else if (method == query_interface) {
                EXPECT_EQ(count, 3u);
                EXPECT_EQ(args[0], persist);
                EXPECT_EQ(args[1], requested_iid);
                EXPECT_TRUE(memory.write_u32(
                    args[2], requested_object).is_ok());
            } else if (method == release) {
                EXPECT_EQ(count, 1u);
                EXPECT_EQ(args[0], persist);
            }
            return static_cast<u64>(com::S_OK);
        });

    auto exports = create_com_core_thunks(manager);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "OleLoadFromStream";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 12u);

    set_args({static_cast<u32>(stream), static_cast<u32>(requested_iid),
              static_cast<u32>(out_ptr)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(memory.read_u32(out_ptr).value(), requested_object);
    EXPECT_EQ(calls, (std::vector<GuestAddr>{
        read, co_create.value(), load, query_interface, release}));
}
