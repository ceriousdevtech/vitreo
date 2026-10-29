// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_shell_link.cpp
/// @brief IShellLink encoding, concurrency and x64 vtable tests.

#include "com_test_common.hpp"
#include "com_test_shell_link_fixture.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>

// Independent objects created by MTA workers share process-owned allocation
// and lookup tables. IUnknown's references must remain balanced per object.
TEST_F(ShellLinkEncodingTest, IndependentObjectsSurviveConcurrentCreation) {
    mgr_.co_uninitialize();
    ASSERT_EQ(mgr_.co_initialize_ex(com::COINIT_MULTITHREADED), com::S_OK);
    write_guid(CLSID_ADDR, CLSID_SHELLLINK);
    write_guid(IID_ADDR, IID_ISHELLLINKA);
    write_guid(QI_IID_ADDR, IID_IPERSISTFILE);
    for (bool wide : {false, true}) {
        constexpr unsigned workers = 8, iterations = 128;
        std::array<std::vector<GuestAddr>, workers> objects;
        std::vector<std::thread> threads;
        std::barrier start(workers);
        for (unsigned worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                cpu::CpuState cpu;
                if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
                const GuestAddr frame = STACK_BASE + 0x80 + worker * 0x100;
                const GuestAddr data = DATA_BASE + 0x1000 + worker * 0x800;
                cpu.set_sp(frame);
                if (wide) cpu.seg.gs_desc.base = data + 0x400;
                else cpu.seg.fs_desc.base = data + 0x400;
                Marshaler m(memory_);
                m.attach_cpu(cpu);
                auto args = [&](std::initializer_list<u64> values) {
                    unsigned i = 0;
                    for (u64 value : values) {
                        if (!wide) EXPECT_TRUE(memory_.write_u32(frame + 4 + 4 * i, value).is_ok());
                        else if (i == 0) cpu.gpr.rcx = value;
                        else if (i == 1) cpu.gpr.rdx = value;
                        else if (i == 2) cpu.gpr.r8 = value;
                        else if (i == 3) cpu.gpr.r9 = value;
                        else EXPECT_TRUE(memory_.write_u64(frame + 0x28 + 8 * (i - 4), value).is_ok());
                        ++i;
                    }
                };
                auto pointer = [&](GuestAddr address) -> GuestAddr {
                    return wide ? memory_.read_u64(address).value_or(0)
                                : memory_.read_u32(address).value_or(0);
                };
                start.arrive_and_wait();
                for (unsigned n = 0; n < iterations; ++n) {
                    args({CLSID_ADDR, 0, 1, IID_ADDR, data});
                    ASSERT_EQ(mgr_.co_create_instance(m), com::S_OK);
                    const GuestAddr object = pointer(data);
                    ASSERT_NE(object, 0u);
                    objects[worker].push_back(object);
                    const GuestAddr vtable = pointer(object);
                    EXPECT_EQ(vtable, object + (wide ? 8 : 4));
                    for (unsigned slot = 0; slot < 21; ++slot)
                        EXPECT_NE(pointer(vtable + slot * (wide ? 8 : 4)), 0u);
                    const std::string path = "C:\\object-" + std::to_string(worker) + "-" + std::to_string(n);
                    ASSERT_TRUE(memory_.write_bytes(data + 0x80, reinterpret_cast<const Byte*>(path.c_str()), path.size() + 1).is_ok());
                    args({object, data + 0x80});
                    EXPECT_EQ(mgr_.shelllink_set_path(m), com::S_OK);
                    args({object, data + 0x200, 256, 0, 0});
                    EXPECT_EQ(mgr_.shelllink_get_path(m), com::S_OK);
                    EXPECT_EQ(m.read_ansi_string(data + 0x200).value_or(""), path);
                    args({object, QI_IID_ADDR, data + 8});
                    EXPECT_EQ(mgr_.shelllink_query_interface(m), com::S_OK);
                    const GuestAddr persist = pointer(data + 8);
                    EXPECT_NE(persist, object);
                    args({persist});
                    EXPECT_EQ(mgr_.shelllink_release(m), 1u);
                    args({object});
                    EXPECT_EQ(mgr_.shelllink_add_ref(m), 2u);
                    EXPECT_EQ(mgr_.shelllink_release(m), 1u);
                    EXPECT_EQ(mgr_.shelllink_release(m), 0u);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        std::vector<GuestAddr> all;
        for (auto& addresses : objects) all.insert(all.end(), addresses.begin(), addresses.end());
        ASSERT_EQ(all.size(), workers * iterations);
        std::sort(all.begin(), all.end());
        EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end());
    }
}

TEST_F(ShellLinkEncodingTest, InvalidScratchArenaFailsWithNullOutput) {
    const Guid CLSID_MMDeviceEnumerator{0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    const Guid IID_IMMDeviceEnumerator{0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
    write_guid(CLSID_ADDR, CLSID_SHELLLINK);
    write_guid(IID_ADDR, IID_ISHELLLINKA);
    mgr_.set_scratch_base(std::numeric_limits<GuestAddr>::max() - 1);
    for (const auto& ids : {std::pair{CLSID_SHELLLINK, IID_ISHELLLINKA},
                            std::pair{CLSID_MMDeviceEnumerator, IID_IMMDeviceEnumerator}}) {
        write_guid(CLSID_ADDR, ids.first);
        write_guid(IID_ADDR, ids.second);
        for (bool wide : {false, true}) {
            if (wide) cpu_.init_long_mode(); else cpu_.init_protected_mode();
            cpu_.set_sp(STACK_BASE + 0x400);
            if (wide) {
                cpu_.gpr.rcx = CLSID_ADDR; cpu_.gpr.rdx = 0;
                cpu_.gpr.r8 = 1; cpu_.gpr.r9 = IID_ADDR;
                ASSERT_TRUE(memory_.write_u64(cpu_.sp() + 0x28, OUT_ADDR).is_ok());
            } else set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
            ASSERT_TRUE(memory_.write_u64(OUT_ADDR, ~u64{0}).is_ok());
            auto m = make_marshaler();
            // SDK winerror.h E_OUTOFMEMORY; CoCreateInstance clears *ppv on failure.
            EXPECT_EQ(mgr_.co_create_instance(m), 0x8007000Eu);
            EXPECT_EQ(wide ? memory_.read_u64(OUT_ADDR).value_or(1)
                           : memory_.read_u32(OUT_ADDR).value_or(1), 0u);
        }
    }
}

TEST_F(ShellLinkEncodingTest, MissingShortcutLoadDoesNotReportSuccess) {
    write_guid(CLSID_ADDR, CLSID_SHELLLINK);
    write_guid(IID_ADDR, IID_IPERSISTFILE);
    for (bool wide : {false, true}) {
        if (wide) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        cpu_.set_sp(STACK_BASE + 0x400);
        if (wide) {
            cpu_.gpr.rcx = CLSID_ADDR;
            cpu_.gpr.rdx = 0;
            cpu_.gpr.r8 = 1;
            cpu_.gpr.r9 = IID_ADDR;
            ASSERT_TRUE(memory_.write_u64(cpu_.sp() + 0x28, OUT_ADDR).is_ok());
        } else set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
        auto m = make_marshaler();
        ASSERT_EQ(mgr_.co_create_instance(m), com::S_OK);
        const GuestAddr persist = wide ? memory_.read_u64(OUT_ADDR).value_or(0)
                                      : memory_.read_u32(OUT_ADDR).value_or(0);
        ASSERT_NE(persist, 0u);
        ASSERT_TRUE(m.write_wide_string(SAVE_PATH_ADDR, "C:\\missing.lnk").is_ok());
        for (u32 mode : {0x00000000u, 0x00000001u, 0x00000002u}) { // SDK STGM_READ/WRITE/READWRITE.
            if (wide) {
                cpu_.gpr.rcx = persist;
                cpu_.gpr.rdx = SAVE_PATH_ADDR;
                cpu_.gpr.r8 = mode;
            } else set_args({static_cast<u32>(persist), SAVE_PATH_ADDR, mode});
            // Learn: E_FAIL when the object cannot load for a non-memory reason.
            EXPECT_EQ(mgr_.shelllink_persist_load(m), 0x80004005u);
        }
        if (wide) cpu_.gpr.rcx = persist;
        else set_args({static_cast<u32>(persist)});
        EXPECT_EQ(mgr_.shelllink_release(m), 0u);
    }
}

TEST_F(ShellLinkEncodingTest, AnsiAndUnicodeInterfacesMarshalTheirOwnStringABI) {
    write_guid(CLSID_ADDR, CLSID_SHELLLINK);
    write_guid(IID_ADDR, IID_ISHELLLINKA);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 ansi_iface = read_u32(OUT_ADDR);
    ASSERT_NE(ansi_iface, 0u);

    constexpr std::string_view target =
        "C:\\Program Files (x86)\\Starcraft\\StarCraft.exe";
    auto string_marshaler = make_marshaler();
    ASSERT_TRUE(string_marshaler.write_ansi_string(ANSI_PATH_ADDR, target).is_ok());
    set_args({ansi_iface, ANSI_PATH_ADDR});
    auto set_path_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.shelllink_set_path(set_path_marshaler), com::S_OK);

    constexpr GuestAddr GET_PATH_ADDR = DATA_BASE + 0x700;
    set_args({ansi_iface, GET_PATH_ADDR, 260, 0, 0});
    auto get_path_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.shelllink_get_path(get_path_marshaler), com::S_OK);
    std::string got;
    for (u32 i = 0; i < 260; ++i) {
        const auto ch = memory_.read_u8(GET_PATH_ADDR + i);
        ASSERT_TRUE(ch.is_ok());
        if (ch.value() == 0) break;
        got.push_back(static_cast<char>(ch.value()));
    }
    EXPECT_EQ(got, target);

    write_guid(QI_IID_ADDR, IID_ISHELLLINKW);
    set_args({ansi_iface, QI_IID_ADDR, QI_OUT_ADDR});
    auto wide_qi_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.shelllink_query_interface(wide_qi_marshaler), com::S_OK);
    const u32 unicode_iface = read_u32(QI_OUT_ADDR);
    ASSERT_NE(unicode_iface, 0u);
    EXPECT_NE(unicode_iface, ansi_iface);

    constexpr std::string_view working_dir =
        "C:\\Program Files (x86)\\Starcraft";
    auto wide_string_marshaler = make_marshaler();
    ASSERT_TRUE(
        wide_string_marshaler.write_wide_string(WIDE_DIR_ADDR, working_dir).is_ok());
    set_args({unicode_iface, WIDE_DIR_ADDR});
    auto set_dir_marshaler = make_marshaler();
    ASSERT_EQ(
        mgr_.shelllink_set_working_directory(set_dir_marshaler), com::S_OK);

    write_guid(QI_IID_ADDR, IID_IPERSISTFILE);
    set_args({ansi_iface, QI_IID_ADDR, QI_OUT_ADDR});
    auto persist_qi_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.shelllink_query_interface(persist_qi_marshaler), com::S_OK);
    const u32 persist_iface = read_u32(QI_OUT_ADDR);
    ASSERT_NE(persist_iface, 0u);
    const u32 persist_vtable = read_u32(persist_iface);
    ASSERT_EQ(
        read_u32(persist_vtable + 4u * sizeof(u32)),
        registry_.get_thunk_address(
            "ole32.dll", "Vitreo_ShellLink_PersistIsDirty").value());
    ASSERT_EQ(
        read_u32(persist_vtable + 5u * sizeof(u32)),
        registry_.get_thunk_address(
            "ole32.dll", "Vitreo_ShellLink_PersistLoad").value());
    ASSERT_EQ(
        read_u32(persist_vtable + 6u * sizeof(u32)),
        registry_.get_thunk_address(
            "ole32.dll", "Vitreo_ShellLink_PersistSave").value());
    ASSERT_EQ(
        read_u32(persist_vtable + 7u * sizeof(u32)),
        registry_.get_thunk_address(
            "ole32.dll", "Vitreo_ShellLink_PersistSaveCompleted").value());
    ASSERT_EQ(
        read_u32(persist_vtable + 8u * sizeof(u32)),
        registry_.get_thunk_address(
            "ole32.dll", "Vitreo_ShellLink_PersistGetCurFile").value());

    auto save_string_marshaler = make_marshaler();
    ASSERT_TRUE(save_string_marshaler.write_wide_string(
        SAVE_PATH_ADDR, "C:\\Programs\\Starcraft.lnk").is_ok());
    set_args({persist_iface, SAVE_PATH_ADDR, 1});
    auto save_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.shelllink_persist_save(save_marshaler), com::S_OK);

    std::ifstream shortcut(test_root_ + "/Programs/Starcraft.lnk",
                           std::ios::binary);
    ASSERT_TRUE(shortcut.is_open());
    const std::string payload{
        std::istreambuf_iterator<char>(shortcut),
        std::istreambuf_iterator<char>()};
    ASSERT_GT(payload.size(), 80u);
    u32 header_size = 0;
    u32 link_flags = 0;
    std::memcpy(&header_size, payload.data(), sizeof(header_size));
    std::memcpy(&link_flags, payload.data() + 20, sizeof(link_flags));
    EXPECT_EQ(header_size, 0x4Cu);
    EXPECT_EQ(link_flags, 0x00000098u);  // RelativePath, WorkingDir, Unicode
    EXPECT_EQ(
        std::string_view(payload).substr(payload.size() - 4),
        std::string_view("\0\0\0\0", 4));
}

TEST(ShellLinkX64Test, CoCreateInstanceVtableUsesFull64BitPointers) {
    // Chrome setup CoCreateInstance(CLSID_ShellLink) then QI IPersistFile via
    // mov rax,[rcx]; mov rax,[rax]. 32-bit vtable slots splice object+QI.
    constexpr GuestAddr kData = 0x00100000;
    constexpr GuestAddr kStack = 0x00200000;
    constexpr GuestAddr kCom = ComCoreManager::COM_SCRATCH_BASE;
    constexpr GuestAddr kThunkBase = 0x7FFB00000000ull;
    constexpr GuestAddr kClsid = kData;
    constexpr GuestAddr kIid = kData + 0x20;
    constexpr GuestAddr kPersistIid = kData + 0x40;
    constexpr GuestAddr kOut = kData + 0x80;
    constexpr GuestAddr kQiOut = kData + 0x90;

    constexpr Guid kClsidShellLink{
        0x00021401u, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    constexpr Guid kIidShellLinkW{
        0x000214F9u, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    constexpr Guid kIidPersistFile{
        0x0000010Bu, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kData, 0x10000, MemoryProtection::ReadWrite)
                    .is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite)
                    .is_ok());
    ASSERT_TRUE(memory.map_region(kCom, 0x10000, MemoryProtection::ReadWrite)
                    .is_ok());

    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_manager{vmm, memory, handles};
    ComTaskMemoryManager task_memory{heap_manager};
    ComCoreManager mgr{task_memory};
    ThunkRegistry registry{kThunkBase};
    ASSERT_TRUE(
        registry.register_dll("ole32.dll", create_com_core_thunks(mgr)).is_ok());
    mgr.set_thunk_registry(&registry);
    (void)mgr.co_initialize();  // CoCreateInstance needs an apartment (measured)

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x400;

    ASSERT_TRUE(memory
                    .write_bytes(kClsid,
                                 reinterpret_cast<const Byte*>(&kClsidShellLink),
                                 sizeof(kClsidShellLink))
                    .is_ok());
    ASSERT_TRUE(memory
                    .write_bytes(kIid,
                                 reinterpret_cast<const Byte*>(&kIidShellLinkW),
                                 sizeof(kIidShellLinkW))
                    .is_ok());
    ASSERT_TRUE(memory
                    .write_bytes(
                        kPersistIid,
                        reinterpret_cast<const Byte*>(&kIidPersistFile),
                        sizeof(kIidPersistFile))
                    .is_ok());

    cpu.gpr.rcx = kClsid;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 1;  // CLSCTX_INPROC_SERVER
    cpu.gpr.r9 = kIid;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kOut).is_ok());

    Marshaler create_m{memory};
    create_m.attach_cpu(cpu);
    ASSERT_EQ(mgr.co_create_instance(create_m), com::S_OK);

    auto iface_r = memory.read_u64(kOut);
    ASSERT_TRUE(iface_r.is_ok());
    const GuestAddr iface = static_cast<GuestAddr>(iface_r.value());
    ASSERT_NE(iface, 0u);

    auto vtable_r = memory.read_u64(iface);
    ASSERT_TRUE(vtable_r.is_ok());
    const GuestAddr vtable = static_cast<GuestAddr>(vtable_r.value());
    EXPECT_EQ(vtable, iface + 8);

    auto qi_r = memory.read_u64(vtable);
    ASSERT_TRUE(qi_r.is_ok());
    EXPECT_GE(qi_r.value(), kThunkBase);
    EXPECT_EQ(qi_r.value() & 0xFFFFFFFF00000000ull, kThunkBase);

    cpu.gpr.rcx = iface;
    cpu.gpr.rdx = kPersistIid;
    cpu.gpr.r8 = kQiOut;
    Marshaler qi_m{memory};
    qi_m.attach_cpu(cpu);
    ASSERT_EQ(mgr.shelllink_query_interface(qi_m), com::S_OK);
    auto persist_r = memory.read_u64(kQiOut);
    ASSERT_TRUE(persist_r.is_ok());
    EXPECT_NE(persist_r.value(), 0u);
    EXPECT_NE(persist_r.value(), iface);

    // IPropertyStore: object + 8-slot vtable of full 64-bit thunk pointers
    // (QI, AddRef, Release, GetCount, GetAt, GetValue, SetValue, Commit).
    constexpr Guid kIidPropertyStore{
        0x886D8EEBu, 0x8CF2u, 0x4446u, {0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99}};
    ASSERT_TRUE(memory.write_bytes(kPersistIid, reinterpret_cast<const Byte*>(&kIidPropertyStore),
                                   sizeof(kIidPropertyStore)).is_ok());
    cpu.gpr.rcx = iface;
    cpu.gpr.rdx = kPersistIid;
    cpu.gpr.r8 = kQiOut;
    Marshaler ps_m{memory};
    ps_m.attach_cpu(cpu);
    ASSERT_EQ(mgr.shelllink_query_interface(ps_m), com::S_OK);
    const GuestAddr ps = static_cast<GuestAddr>(memory.read_u64(kQiOut).value_or(0));
    ASSERT_NE(ps, 0u);
    EXPECT_NE(ps, persist_r.value());
    const GuestAddr ps_vtable = static_cast<GuestAddr>(memory.read_u64(ps).value_or(0));
    EXPECT_EQ(ps_vtable, ps + 8);
    for (u32 slot = 0; slot < 8; ++slot) {
        EXPECT_EQ(memory.read_u64(ps_vtable + slot * 8).value_or(0) & 0xFFFFFFFF00000000ull,
                  kThunkBase) << slot;
    }
}

// IPropertyStore on CLSID_ShellLink and the saved PropertyStoreDataBlock.
// Expected results and record bytes: tools/oracle_probe/
// shelllink_propstore_probe32.expected ("aumid", "committed", "type_i8",
// get_*/set_*/getat_oob lines). Windows also writes a creator-SID storage and
// a volume-GUID storage that Vitreo does not produce, and orders values by an
// internal hash, so the block is compared per FMTID and per value record.
TEST_F(ShellLinkEncodingTest, PropertyStoreMatchesWindowsAndSavesItsBlock) {
    static constexpr Guid IID_IPROPERTYSTORE{
        0x886D8EEBu, 0x8CF2u, 0x4446u, {0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99}};
    static constexpr Guid kAumFmtid{
        0x9F4C2855u, 0x9F79u, 0x4B39u, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}};
    static constexpr Guid kOtherFmtid{
        0x12345678u, 0x1234u, 0x5678u, {1, 2, 3, 4, 5, 6, 7, 8}};
    static constexpr Guid kToast{
        0xAABBCCDDu, 0x1122u, 0x3344u, {0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC}};
    constexpr GuestAddr KEY = DATA_BASE + 0x2000;   // PROPERTYKEY
    constexpr GuestAddr PV = DATA_BASE + 0x2040;    // PROPVARIANT in
    constexpr GuestAddr OUT_PV = DATA_BASE + 0x2080;
    constexpr GuestAddr STR = DATA_BASE + 0x2100;
    constexpr GuestAddr CLSID = DATA_BASE + 0x2200;
    constexpr GuestAddr COUNT = DATA_BASE + 0x2300;
    constexpr GuestAddr OUT_KEY = DATA_BASE + 0x2320;

    write_guid(CLSID_ADDR, CLSID_SHELLLINK);
    write_guid(IID_ADDR, IID_ISHELLLINKW);
    write_guid(QI_IID_ADDR, IID_IPROPERTYSTORE);
    auto m = make_marshaler();
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    ASSERT_EQ(mgr_.co_create_instance(m), com::S_OK);
    const u32 link = read_u32(OUT_ADDR);
    ASSERT_TRUE(m.write_wide_string(WIDE_DIR_ADDR, "C:\\Windows\\notepad.exe").is_ok());
    set_args({link, WIDE_DIR_ADDR});
    ASSERT_EQ(mgr_.shelllink_set_path(m), com::S_OK);
    set_args({link, QI_IID_ADDR, QI_OUT_ADDR});
    ASSERT_EQ(mgr_.shelllink_query_interface(m), com::S_OK);   // qi_propertystore
    const u32 ps = read_u32(QI_OUT_ADDR);
    ASSERT_NE(ps, 0u);
    ASSERT_NE(ps, link);

    auto key = [&](const Guid& fmtid, u32 pid) {
        write_guid(KEY, fmtid);
        ASSERT_TRUE(memory_.write_u32(KEY + 16, pid).is_ok());
    };
    auto pv = [&](u16 vt, u64 value) {
        for (u32 off = 0; off < 16; off += 4) ASSERT_TRUE(memory_.write_u32(PV + off, 0).is_ok());
        ASSERT_TRUE(memory_.write_u16(PV, vt).is_ok());
        ASSERT_TRUE(memory_.write_u32(PV + 8, static_cast<u32>(value)).is_ok());
        ASSERT_TRUE(memory_.write_u32(PV + 12, static_cast<u32>(value >> 32)).is_ok());
    };
    auto set = [&]() { set_args({ps, KEY, PV}); return mgr_.shelllink_propstore_set_value(m); };
    auto get = [&]() {
        for (u32 off = 0; off < 16; off += 4) (void)memory_.write_u32(OUT_PV + off, 0xCCCCCCCCu);
        set_args({ps, KEY, OUT_PV});
        return mgr_.shelllink_propstore_get_value(m);
    };
    auto count = [&]() {
        set_args({ps, COUNT});
        EXPECT_EQ(mgr_.shelllink_propstore_get_count(m), com::S_OK);
        return read_u32(COUNT);
    };

    // get_unset_aumid: S_OK, VT_EMPTY.
    key(kAumFmtid, 5);
    EXPECT_EQ(get(), com::S_OK);
    EXPECT_EQ(memory_.read_u16(OUT_PV).value_or(9), 0u);
    EXPECT_EQ(count(), 0u);  // Windows: 1 (volume GUID property; not modelled)

    // set_aumid_lpwstr / get_aumid: a copy, not the caller's buffer.
    ASSERT_TRUE(m.write_wide_string(STR, "Microsoft.VisualStudioCode").is_ok());
    pv(31, STR);
    EXPECT_EQ(set(), com::S_OK);
    EXPECT_EQ(count(), 1u);
    EXPECT_EQ(get(), com::S_OK);
    EXPECT_EQ(memory_.read_u16(OUT_PV).value_or(0), 31u);
    const GuestAddr copy = read_u32(OUT_PV + 8);
    EXPECT_NE(copy, STR);
    EXPECT_EQ(m.read_wide_string(copy).value_or(""), "Microsoft.VisualStudioCode");

    // set_preventpin (VT_BOOL), set_toast (VT_CLSID), other key (VT_I8).
    key(kAumFmtid, 9);
    pv(11, 0xFFFF);
    EXPECT_EQ(set(), com::S_OK);
    write_guid(CLSID, kToast);
    key(kAumFmtid, 26);
    pv(72, CLSID);
    EXPECT_EQ(set(), com::S_OK);
    key(kOtherFmtid, 2);
    pv(20, 0x0102030405060708ull);
    EXPECT_EQ(set(), com::S_OK);
    // set_other_empty: VT_EMPTY keeps the key in GetCount.
    key(kOtherFmtid, 3);
    pv(0, 0);
    EXPECT_EQ(set(), com::S_OK);
    EXPECT_EQ(count(), 5u);

    // getat_oob: E_INVALIDARG with the key zeroed.
    for (u32 off = 0; off < 20; off += 4) (void)memory_.write_u32(OUT_KEY + off, 0xCCCCCCCCu);
    set_args({ps, 5, OUT_KEY});
    EXPECT_EQ(mgr_.shelllink_propstore_get_at(m), 0x80070057u);
    EXPECT_EQ(read_u32(OUT_KEY + 16), 0u);
    EXPECT_EQ(read_u32(OUT_KEY), 0u);
    set_args({ps, 0, OUT_KEY});
    EXPECT_EQ(mgr_.shelllink_propstore_get_at(m), com::S_OK);
    EXPECT_EQ(read_u32(OUT_KEY + 16), 5u);

    // commit (twice), then QI back to IPersistFile from the property store.
    set_args({ps});
    EXPECT_EQ(mgr_.shelllink_propstore_commit(m), com::S_OK);
    EXPECT_EQ(mgr_.shelllink_propstore_commit(m), com::S_OK);
    write_guid(QI_IID_ADDR, IID_IPERSISTFILE);
    set_args({ps, QI_IID_ADDR, QI_OUT_ADDR});
    ASSERT_EQ(mgr_.shelllink_query_interface(m), com::S_OK);   // ps_qi_persistfile
    const u32 persist = read_u32(QI_OUT_ADDR);
    ASSERT_TRUE(m.write_wide_string(SAVE_PATH_ADDR, "C:\\props.lnk").is_ok());
    set_args({persist, SAVE_PATH_ADDR, 1});
    ASSERT_EQ(mgr_.shelllink_persist_save(m), com::S_OK);

    // Parse the saved PropertyStoreDataBlock.
    std::ifstream file(test_root_ + "/props.lnk", std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(file)), {});
    auto u32_at = [&](usize at) {
        return static_cast<u32>(bytes[at]) | (static_cast<u32>(bytes[at + 1]) << 8) |
               (static_cast<u32>(bytes[at + 2]) << 16) | (static_cast<u32>(bytes[at + 3]) << 24);
    };
    usize block = 0;
    for (usize i = 4; i + 4 <= bytes.size(); ++i) {
        if (u32_at(i) == 0xA0000009u) { block = i - 4; break; }
    }
    ASSERT_NE(block, 0u);
    auto hex = [&](usize at, usize n) {
        std::string s;
        char b[3];
        for (usize i = 0; i < n; ++i) { std::snprintf(b, sizeof b, "%02x", bytes[at + i]); s += b; }
        return s;
    };
    std::map<std::string, std::set<std::string>> storages;
    usize at = block + 8;
    while (u32_at(at) != 0) {
        const u32 size = u32_at(at);
        EXPECT_EQ(u32_at(at + 4), 0x53505331u);
        auto& records = storages[hex(at + 8, 16)];
        usize v = at + 24;
        while (u32_at(v) != 0) { records.insert(hex(v, u32_at(v))); v += u32_at(v); }
        at += size;
    }
    EXPECT_EQ(u32_at(block), static_cast<u32>(at + 4 - block));   // BlockSize
    EXPECT_EQ(u32_at(at + 4), 0u);                                 // TerminalBlock

    // "aumid" + "committed" blocks (AppUserModel FMTID records).
    const std::set<std::string> aum = {
        "4900000005000000001f0000001b0000004d006900630072006f0073006f00660074002e0056006900730075"
        "0061006c00530074007500640069006f0043006f006400650000000000",
        "1100000009000000000b000000ffff0000",
        "1d0000001a0000000048000000ddccbbaa221144335566778899aabbcc",
    };
    EXPECT_EQ(storages["55284c9f799f394ba8d0e1d42de1d5f3"], aum);
    // "type_i8" record; the VT_EMPTY value is not written.
    const std::set<std::string> other = {"150000000200000000140000000807060504030201"};
    EXPECT_EQ(storages["78563412341278560102030405060708"], other);
    EXPECT_EQ(storages.size(), 2u);

    set_args({ps});
    EXPECT_EQ(mgr_.shelllink_release(m), 2u);
    set_args({persist});
    EXPECT_EQ(mgr_.shelllink_release(m), 1u);
    set_args({link});
    EXPECT_EQ(mgr_.shelllink_release(m), 0u);
}
