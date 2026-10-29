// SPDX-License-Identifier: LGPL-2.1-only

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include <climits>
#include <unistd.h>

#include "winapi/kernel32/memory/remote_memory.hpp"
#include "core/memory/guest_memory.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"

using namespace vitreo;

namespace {

constexpr GuestAddr kBase = 0x24000;
constexpr usize kRegionSize = 0x1000;

TEST(RemoteMemory, ReadsAndWritesActualGuestMemory) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        kBase, kRegionSize, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kBase + 0x20, 0x12345678u).is_ok());

    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);
    ASSERT_TRUE(server.start(false));

    u32 observed = 0;
    auto output = std::span<Byte>(
        reinterpret_cast<Byte*>(&observed), sizeof(observed));
    ASSERT_TRUE(ipc::remote_read_process_memory(
        ::getpid(), kBase + 0x20, output));
    EXPECT_EQ(observed, 0x12345678u);

    const u32 replacement = 0xA1B2C3D4u;
    const auto input = std::span<const Byte>(
        reinterpret_cast<const Byte*>(&replacement), sizeof(replacement));
    ASSERT_TRUE(ipc::remote_write_process_memory(
        ::getpid(), kBase + 0x20, input));

    const auto stored = memory.read_u32(kBase + 0x20);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), replacement);
}

TEST(RemoteMemory, SuspendedPrimaryThreadWaitsForResumeRequest) {
    GuestMemory memory;
    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);
    ASSERT_TRUE(server.start(true));

    std::atomic<bool> resumed{false};
    std::thread waiter([&] {
        server.wait_for_initial_resume();
        resumed.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(resumed.load(std::memory_order_acquire));
    EXPECT_TRUE(ipc::remote_resume_primary_thread(::getpid()));
    waiter.join();
    EXPECT_TRUE(resumed.load(std::memory_order_acquire));
}

TEST(RemoteMemory, CallsGuestThroughOwningProcessEndpoint) {
    GuestMemory memory;
    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);
    server.set_guest_call_handler(
        [](GuestAddr function,
           std::span<const u64> arguments,
           bool caller_elevated) -> std::optional<u64> {
            if (function != 0x12345000 || arguments.size() != 3) {
                return std::nullopt;
            }
            if (!caller_elevated) return std::nullopt;
            return arguments[0] + arguments[1] + arguments[2];
        });
    ASSERT_TRUE(server.start(false));

    const std::array<u64, 3> arguments{10, 20, 12};
    const auto result = ipc::remote_call_guest(
        ::getpid(), 0x12345000, arguments, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42u);
    EXPECT_FALSE(ipc::remote_call_guest(::getpid(), 0, arguments).has_value());
}

TEST(RemoteMemory, ShowWindowRunsOnTheOwningProcessEndpoint) {
    // ShowWindow on another process's window (Learn: any HWND of the desktop)
    // is carried out by its owner, whose ShowWindow return comes back.
    GuestMemory memory;
    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);
    u32 seen_hwnd = 0;
    i32 seen_cmd = -1;
    server.set_window_show_handler([&](u32 hwnd, i32 cmd) -> std::optional<u64> {
        if (hwnd != 0x5c37d) return std::nullopt;  // not a window here
        seen_hwnd = hwnd;
        seen_cmd = cmd;
        return 0x18;  // previously visible
    });
    ASSERT_TRUE(server.start(false));
    u64 result = 0;
    ASSERT_TRUE(ipc::remote_show_window(::getpid(), 0x5c37d, 5, result));
    EXPECT_EQ(seen_hwnd, 0x5c37du);
    EXPECT_EQ(seen_cmd, 5);
    EXPECT_EQ(result, 0x18u);
    EXPECT_FALSE(ipc::remote_show_window(::getpid(), 0x5c399, 5, result));
}

TEST(RemoteMemory, MaterializesPeerComInterfaceThroughEndpoint) {
    GuestMemory memory;
    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);
    const std::array<Byte, 16> iid{
        0x21, 0x43, 0x65, 0x87, 0xBA, 0xDC, 0x34, 0x12,
        0x90, 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76};
    server.set_com_interface_marshal_handler(
        [&](u32 owner_pid, GuestAddr object, u32 pointer_size,
            std::span<const Byte> requested_iid) -> GuestAddr {
            EXPECT_EQ(owner_pid, 1234u);
            EXPECT_EQ(object, 0x00456000u);
            EXPECT_EQ(pointer_size, 4u);
            EXPECT_TRUE(std::equal(
                requested_iid.begin(), requested_iid.end(), iid.begin()));
            return 0x34567000u;
        });
    ASSERT_TRUE(server.start(false));

    EXPECT_EQ(ipc::remote_marshal_com_interface(
                  ::getpid(), 1234, 0x00456000, 4, iid),
              0x34567000u);
}

TEST(RemoteMemory, SuspendedProcessAllowsPebShimWriteBeforeResume) {
    // Chromium ApplyZeroAppShimToSuspendedProcess zeros PEB.pShimData
    // (x86 +0x1e8) via WriteProcessMemory while CREATE_SUSPENDED. The PEB
    // page must already be mapped when the remote-memory endpoint is live.
    constexpr GuestAddr kPeb = 0x7FFE2000;
    constexpr u32 kShimDataOffset = 0x1e8;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        kPeb, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kPeb + kShimDataOffset, 0xDEADBEEFu).is_ok());

    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);
    ASSERT_TRUE(server.start(true));

    std::atomic<bool> resumed{false};
    std::thread waiter([&] {
        server.wait_for_initial_resume();
        resumed.store(true, std::memory_order_release);
    });

    const u32 zero = 0;
    const auto input = std::span<const Byte>(
        reinterpret_cast<const Byte*>(&zero), sizeof(zero));
    ASSERT_TRUE(ipc::remote_write_process_memory(
        ::getpid(), kPeb + kShimDataOffset, input));
    EXPECT_FALSE(resumed.load(std::memory_order_acquire));
    EXPECT_EQ(memory.read_u32(kPeb + kShimDataOffset).value(), 0u);

    EXPECT_TRUE(ipc::remote_resume_primary_thread(::getpid()));
    waiter.join();
    EXPECT_TRUE(resumed.load(std::memory_order_acquire));
}

TEST(RemoteMemory, RejectsUnmappedGuestRange) {
    GuestMemory memory;
    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);
    ASSERT_TRUE(server.start(false));

    u32 observed = 0;
    auto output = std::span<Byte>(
        reinterpret_cast<Byte*>(&observed), sizeof(observed));
    EXPECT_FALSE(ipc::remote_read_process_memory(
        ::getpid(), kBase, output));
}

TEST(RemoteMemory, AllocatesProtectsAndFreesActualGuestMemory) {
    GuestMemory memory;
    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);
    ASSERT_TRUE(server.start(false));

    const GuestAddr allocated = ipc::remote_virtual_alloc(
        ::getpid(), 0, 0x1800,
        winapi::MEM_COMMIT | winapi::MEM_RESERVE,
        winapi::PAGE_READWRITE);
    ASSERT_NE(allocated, 0u);
    EXPECT_TRUE(memory.is_mapped(allocated, 0x2000));

    u32 old_protect = 0;
    EXPECT_TRUE(ipc::remote_virtual_protect(
        ::getpid(), allocated, 0x1000,
        winapi::PAGE_READONLY, old_protect));
    EXPECT_EQ(old_protect, winapi::PAGE_READWRITE);

    EXPECT_TRUE(ipc::remote_virtual_free(
        ::getpid(), allocated, 0, winapi::MEM_RELEASE));
    EXPECT_FALSE(memory.is_mapped(allocated, 0x1000));
}

TEST(RemoteMemory, QueriesMappedAndFreeGuestRegions) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        kBase, 0x2000, MemoryProtection::ReadExecute).is_ok());
    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);
    ASSERT_TRUE(server.start(false));

    const GuestAddr reservation = virtual_memory.virtual_alloc(
        0x400000, 0x3000, winapi::MEM_RESERVE,
        winapi::PAGE_EXECUTE_READWRITE);
    ASSERT_EQ(reservation, 0x400000u);
    ASSERT_EQ(virtual_memory.virtual_alloc(
                  reservation + 0x1000, 0x1000,
                  winapi::MEM_COMMIT, winapi::PAGE_READWRITE),
              reservation + 0x1000);

    ipc::RemoteMemoryBasicInformation mapped{};
    ASSERT_TRUE(ipc::remote_virtual_query(
        ::getpid(), kBase + 0x120, 0xFFFF'FFFFULL, mapped));
    EXPECT_EQ(mapped.base_address, kBase);
    EXPECT_EQ(mapped.allocation_base, kBase);
    EXPECT_EQ(mapped.region_size, 0x2000u);
    EXPECT_EQ(mapped.state, winapi::MEM_COMMIT);
    EXPECT_EQ(mapped.protect, winapi::PAGE_EXECUTE_READ);
    EXPECT_EQ(mapped.type, winapi::MEM_PRIVATE);

    ipc::RemoteMemoryBasicInformation reserved{};
    ASSERT_TRUE(ipc::remote_virtual_query(
        ::getpid(), reservation, 0xFFFF'FFFFULL, reserved));
    EXPECT_EQ(reserved.base_address, reservation);
    EXPECT_EQ(reserved.allocation_base, reservation);
    EXPECT_EQ(reserved.allocation_protect,
              winapi::PAGE_EXECUTE_READWRITE);
    EXPECT_EQ(reserved.region_size, 0x1000u);
    EXPECT_EQ(reserved.state, winapi::MEM_RESERVE);
    EXPECT_EQ(reserved.protect, 0u);
    EXPECT_EQ(reserved.type, winapi::MEM_PRIVATE);

    ipc::RemoteMemoryBasicInformation committed{};
    ASSERT_TRUE(ipc::remote_virtual_query(
        ::getpid(), reservation + 0x1000,
        0xFFFF'FFFFULL, committed));
    EXPECT_EQ(committed.base_address, reservation + 0x1000);
    EXPECT_EQ(committed.allocation_base, reservation);
    EXPECT_EQ(committed.region_size, 0x1000u);
    EXPECT_EQ(committed.state, winapi::MEM_COMMIT);
    EXPECT_EQ(committed.protect, winapi::PAGE_READWRITE);

    ipc::RemoteMemoryBasicInformation free{};
    ASSERT_TRUE(ipc::remote_virtual_query(
        ::getpid(), 0, 0xFFFF'FFFFULL, free));
    EXPECT_EQ(free.base_address, 0u);
    EXPECT_EQ(free.allocation_base, 0u);
    EXPECT_EQ(free.region_size, kBase);
    EXPECT_EQ(free.state, winapi::MEM_FREE);
    EXPECT_EQ(free.protect, winapi::PAGE_NOACCESS);
    EXPECT_EQ(free.type, 0u);
}

TEST(RemoteMemory, ImportsRuntimeDuplicatedEventIntoTargetHandleTable) {
    GuestMemory source_memory;
    GuestMemory target_memory;
    winapi::HandleTable source_handles;
    winapi::HandleTable target_handles;
    winapi::SyncManager source_sync(source_handles);
    winapi::SyncManager target_sync(target_handles);
    winapi::FileMappingManager target_mappings(
        target_handles, target_memory);
    winapi::VirtualMemoryManager target_virtual_memory(target_memory);
    ipc::RemoteMemoryServer server(
        target_memory, target_virtual_memory,
        &target_sync, &target_mappings);
    ASSERT_TRUE(server.start(false));

    const winapi::Handle source_event =
        source_sync.create_event(
            /*manual_reset=*/false, /*initial_state=*/false, "");
    constexpr u32 kSynchronizeAndModifyState = 0x00100002;
    const auto descriptor = source_sync.prepare_transferred_handle(
        source_event, kSynchronizeAndModifyState, /*inheritable=*/false);
    ASSERT_TRUE(descriptor.has_value());

    const winapi::Handle target_event =
        ipc::remote_import_handle(::getpid(), *descriptor);
    ASSERT_NE(target_event, winapi::NULL_HANDLE);
    EXPECT_EQ(target_handles.get_type(target_event),
              winapi::HandleType::Event);
    EXPECT_TRUE(source_sync.set_event(source_event));
    EXPECT_EQ(target_sync.wait_for_single_object(target_event, 0),
              winapi::WAIT_OBJECT_0);
}

TEST(RemoteMemory, ImportsRuntimeDuplicatedSectionWithSharedBytes) {
    GuestMemory source_memory;
    GuestMemory target_memory;
    winapi::HandleTable source_handles;
    winapi::HandleTable target_handles;
    winapi::FileMappingManager source_mappings(
        source_handles, source_memory);
    winapi::FileMappingManager target_mappings(
        target_handles, target_memory);
    winapi::SyncManager target_sync(target_handles);
    winapi::VirtualMemoryManager target_virtual_memory(target_memory);
    ipc::RemoteMemoryServer server(
        target_memory, target_virtual_memory,
        &target_sync, &target_mappings);
    ASSERT_TRUE(server.start(false));

    constexpr u32 kPageReadWrite = 0x04;
    constexpr u32 kFileMapWrite = 0x02;
    constexpr u32 kFileMapRead = 0x04;
    constexpr u32 kSectionQuery = 0x01;
    const winapi::Handle source_section =
        source_mappings.create_file_mapping(
            winapi::FILE_MAPPING_INVALID_FILE, kPageReadWrite,
            0x2000, "");
    ASSERT_NE(source_section, winapi::NULL_HANDLE);
    const GuestAddr source_view = source_mappings.map_view_of_file(
        source_section, kFileMapWrite | kFileMapRead, 0, 0);
    ASSERT_NE(source_view, 0u);
    ASSERT_TRUE(source_memory.write_u32(
        source_view + 0x120, 0xA1B2C3D4u).is_ok());

    const auto descriptor = source_mappings.prepare_transferred_handle(
        source_section,
        kSectionQuery | kFileMapWrite | kFileMapRead,
        /*inheritable=*/false);
    ASSERT_TRUE(descriptor.has_value());
    const winapi::Handle target_section =
        ipc::remote_import_handle(::getpid(), *descriptor);
    ASSERT_NE(target_section, winapi::NULL_HANDLE);
    EXPECT_EQ(target_handles.get_type(target_section),
              winapi::HandleType::FileMapping);

    const GuestAddr target_view = target_mappings.map_view_of_file(
        target_section, kFileMapRead, 0, 0);
    ASSERT_NE(target_view, 0u);
    EXPECT_EQ(target_memory.read_u32(target_view + 0x120).value_or(0),
              0xA1B2C3D4u);
}

TEST(RemoteMemory, ImportsRuntimeDuplicatedFileWithSharedOffset) {
    capsule::Vfs vfs;
    const auto tmp = std::filesystem::temp_directory_path() /
                     "vitreo-remote-file-dup-test";
    std::filesystem::create_directories(tmp);
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(tmp.string())).is_ok());

    GuestMemory source_memory;
    GuestMemory target_memory;
    ASSERT_TRUE(source_memory.map_region(
        kBase, kRegionSize, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(target_memory.map_region(
        kBase, kRegionSize, MemoryProtection::ReadWrite).is_ok());
    winapi::HandleTable source_handles;
    winapi::HandleTable target_handles;
    winapi::FileManager source_files(vfs, source_handles, source_memory);
    winapi::FileManager target_files(vfs, target_handles, target_memory);
    winapi::VirtualMemoryManager target_virtual_memory(target_memory);
    ipc::RemoteMemoryServer server(
        target_memory, target_virtual_memory,
        nullptr, nullptr, &target_handles, &target_files);
    ASSERT_TRUE(server.start(false));

    const auto source = source_files.create_file(
        "C:\\xfer.bin",
        /*GENERIC_READ|GENERIC_WRITE*/ 0xC0000000u,
        /*FILE_SHARE_READ|WRITE*/ 0x3u,
        /*CREATE_ALWAYS*/ 2u,
        0);
    ASSERT_NE(source, winapi::INVALID_HANDLE);

    ASSERT_TRUE(source_memory.write_bytes(
        kBase, reinterpret_cast<const Byte*>("ABCD"), 4).is_ok());
    ASSERT_TRUE(source_files.write_file(source, kBase, 4, kBase + 8));

    int transferred_fd = -1;
    const auto descriptor = source_files.prepare_transferred_handle(
        source, winapi::HandleAccess::All, /*inheritable=*/false,
        transferred_fd);
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_GE(transferred_fd, 0);
    ASSERT_TRUE(descriptor->starts_with("F,"));

    const winapi::Handle target = static_cast<winapi::Handle>(
        ipc::remote_import_handle(::getpid(), *descriptor, transferred_fd));
    ASSERT_NE(target, winapi::NULL_HANDLE);
    EXPECT_EQ(target_handles.get_type(target), winapi::HandleType::File);

    // Shared open-file description: write via the transferred handle continues
    // after the bytes already written through the source handle.
    ASSERT_TRUE(target_memory.write_bytes(
        kBase, reinterpret_cast<const Byte*>("EF"), 2).is_ok());
    ASSERT_TRUE(target_files.write_file(target, kBase, 2, kBase + 8));

    char host_path[PATH_MAX];
    std::snprintf(host_path, sizeof(host_path), "%s/xfer.bin", tmp.c_str());
    FILE* f = std::fopen(host_path, "rb");
    ASSERT_NE(f, nullptr);
    char buf[8]{};
    const auto n = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    EXPECT_EQ(n, 6u);
    EXPECT_EQ(std::string_view(buf, 6), "ABCDEF");

    std::filesystem::remove_all(tmp);
}

TEST(RemoteMemory, ExportsRuntimeDuplicatedFileWithSharedOffset) {
    capsule::Vfs vfs;
    const auto tmp = std::filesystem::temp_directory_path() /
                     "vitreo-remote-file-export-test";
    std::filesystem::create_directories(tmp);
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(tmp.string())).is_ok());

    GuestMemory source_memory;
    GuestMemory target_memory;
    ASSERT_TRUE(source_memory.map_region(
        kBase, kRegionSize, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(target_memory.map_region(
        kBase, kRegionSize, MemoryProtection::ReadWrite).is_ok());
    winapi::HandleTable source_handles;
    winapi::HandleTable target_handles;
    winapi::FileManager source_files(vfs, source_handles, source_memory);
    winapi::FileManager target_files(vfs, target_handles, target_memory);
    winapi::VirtualMemoryManager source_virtual_memory(source_memory);
    ipc::RemoteMemoryServer server(
        source_memory, source_virtual_memory,
        nullptr, nullptr, &source_handles, &source_files);
    ASSERT_TRUE(server.start(false));

    const auto source = source_files.create_file(
        "C:\\xfer-export.bin",
        /*GENERIC_READ|GENERIC_WRITE*/ 0xC0000000u,
        /*FILE_SHARE_READ|WRITE*/ 0x3u,
        /*CREATE_ALWAYS*/ 2u,
        0);
    ASSERT_NE(source, winapi::INVALID_HANDLE);

    ASSERT_TRUE(source_memory.write_bytes(
        kBase, reinterpret_cast<const Byte*>("ABCD"), 4).is_ok());
    ASSERT_TRUE(source_files.write_file(source, kBase, 4, kBase + 8));

    int transferred_fd = -1;
    const auto descriptor = ipc::remote_export_handle(
        ::getpid(), source, /*desired_access=*/0,
        /*same_access=*/true, /*close_source=*/false, &transferred_fd);
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(descriptor->starts_with("F,"));
    ASSERT_GE(transferred_fd, 0);

    const auto target = target_files.import_transferred_handle(
        *descriptor, transferred_fd);
    ASSERT_NE(target, winapi::NULL_HANDLE);
    EXPECT_EQ(target_handles.get_type(target), winapi::HandleType::File);

    ASSERT_TRUE(target_memory.write_bytes(
        kBase, reinterpret_cast<const Byte*>("EF"), 2).is_ok());
    ASSERT_TRUE(target_files.write_file(target, kBase, 2, kBase + 8));

    const auto host_path = tmp / "xfer-export.bin";
    FILE* f = std::fopen(host_path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    char buf[8]{};
    const auto n = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    EXPECT_EQ(n, 6u);
    EXPECT_EQ(std::string_view(buf, 6), "ABCDEF");

    std::filesystem::remove_all(tmp);
}

TEST(RemoteMemory, ImportsRuntimeDuplicatedAnonymousPipeWithSharedBytes) {
    capsule::Vfs vfs;
    GuestMemory source_memory;
    GuestMemory target_memory;
    ASSERT_TRUE(source_memory.map_region(
        kBase, kRegionSize, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(target_memory.map_region(
        kBase, kRegionSize, MemoryProtection::ReadWrite).is_ok());
    winapi::HandleTable source_handles;
    winapi::HandleTable target_handles;
    winapi::FileManager source_files(vfs, source_handles, source_memory);
    winapi::FileManager target_files(vfs, target_handles, target_memory);
    winapi::VirtualMemoryManager target_virtual_memory(target_memory);
    ipc::RemoteMemoryServer server(
        target_memory, target_virtual_memory,
        nullptr, nullptr, &target_handles, &target_files);
    ASSERT_TRUE(server.start(false));

    winapi::Handle read_handle = winapi::NULL_HANDLE;
    winapi::Handle write_handle = winapi::NULL_HANDLE;
    ASSERT_TRUE(source_files.create_anonymous_pipe(
        read_handle, write_handle, false));

    int transferred_fd = -1;
    const auto descriptor = source_files.prepare_transferred_handle(
        write_handle, winapi::HandleAccess::All, /*inheritable=*/false,
        transferred_fd);
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_GE(transferred_fd, 0);
    ASSERT_TRUE(descriptor->starts_with("P,"));

    const winapi::Handle target_write = static_cast<winapi::Handle>(
        ipc::remote_import_handle(::getpid(), *descriptor, transferred_fd));
    ASSERT_NE(target_write, winapi::NULL_HANDLE);
    EXPECT_EQ(target_handles.get_type(target_write), winapi::HandleType::Pipe);

    ASSERT_TRUE(target_memory.write_bytes(
        kBase, reinterpret_cast<const Byte*>("mojo"), 4).is_ok());
    ASSERT_TRUE(target_files.write_file(target_write, kBase, 4, kBase + 8));

    char received[4]{};
    ASSERT_TRUE(source_files.read_file(read_handle, kBase, 4, kBase + 8));
    ASSERT_TRUE(source_memory.read_bytes(
        kBase, reinterpret_cast<Byte*>(received), 4).is_ok());
    EXPECT_EQ(std::string_view(received, 4), "mojo");
}

TEST(RemoteMemory, ExportImportCloseSourceSectionWithoutViews) {
    // Mojo IPCz DecodeHandle(kSender) issues:
    //   DuplicateHandle(remote, section, GetCurrentProcess(), ...,
    //                   SAME_ACCESS | CLOSE_SOURCE)
    // GPU SharedBuffers often encode a section that was never MapViewOfFile'd.
    // Closing the last remote handle before import must not unlink SharedMemory.

    GuestMemory source_memory;
    GuestMemory importer_memory;
    winapi::HandleTable source_handles;
    winapi::HandleTable importer_handles;
    winapi::FileMappingManager source_mappings(
        source_handles, source_memory);
    winapi::FileMappingManager importer_mappings(
        importer_handles, importer_memory);
    winapi::SyncManager source_sync(source_handles);
    winapi::VirtualMemoryManager source_vmm(source_memory);
    source_handles.set_close_callback(
        [&](winapi::Handle h, winapi::HandleType type, void*) {
            if (type == winapi::HandleType::FileMapping) {
                return source_mappings.on_handle_close(h, type);
            }
            return false;
        });

    ipc::RemoteMemoryServer server(
        source_memory, source_vmm, &source_sync, &source_mappings,
        &source_handles, nullptr);
    ASSERT_TRUE(server.start(false));

    constexpr u32 kPageReadWrite = 0x04;
    constexpr u32 kFileMapAll =
        /*SECTION_QUERY|FILE_MAP_WRITE|FILE_MAP_READ*/ 0x7;
    const winapi::Handle source_section =
        source_mappings.create_file_mapping(
            winapi::FILE_MAPPING_INVALID_FILE, kPageReadWrite, 0x10000, "");
    ASSERT_NE(source_section, winapi::NULL_HANDLE);
    // Intentionally no MapViewOfFile — matches GPU SharedBuffer encode.

    auto descriptor = ipc::remote_export_handle(
        ::getpid(), static_cast<u32>(source_section), 0,
        /*same_access=*/true, /*close_source=*/false);
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(descriptor->starts_with("M,"));

    const winapi::Handle imported =
        importer_mappings.import_transferred_handle(*descriptor);
    ASSERT_NE(imported, winapi::NULL_HANDLE)
        << "import must succeed while the remote source handle still pins "
           "SharedMemory lifetime";

    ASSERT_TRUE(ipc::remote_close_handle(
        ::getpid(), static_cast<u32>(source_section)));
    EXPECT_EQ(source_handles.get_type(source_section),
              winapi::HandleType::Invalid);

    // The view has to carry FILE_MAP_WRITE for the store below to be legal:
    // MapViewOfFile(FILE_MAP_READ) yields a PAGE_READONLY view, so writing
    // through it must fail. The test proves the section's shared memory
    // outlives the closed source handle, and a write/read-back round trip is
    // what proves it -- so map with SECTION_QUERY|FILE_MAP_WRITE|FILE_MAP_READ.
    const GuestAddr view = importer_mappings.map_view_of_file(
        imported, kFileMapAll, 0, 0);
    ASSERT_NE(view, 0u);
    ASSERT_TRUE(importer_memory.write_u32(view, 0xC0FFEEu).is_ok());
    EXPECT_EQ(importer_memory.read_u32(view).value_or(0), 0xC0FFEEu);
}

TEST(RemoteMemory, PostsThreadMessageToPeerEndpoint) {
    GuestMemory memory;
    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);

    u32 received_message = 0;
    u64 received_wparam = 0;
    u64 received_lparam = 0;
    server.set_thread_message_handler(
        [&](u32 message, u64 wparam, u64 lparam) {
            received_message = message;
            received_wparam = wparam;
            received_lparam = lparam;
            return true;
        });
    ASSERT_TRUE(server.start(false));

    ASSERT_TRUE(ipc::remote_post_thread_message(
        ::getpid(), 0x8123u, 0x1122334455667788ull,
        0x8877665544332211ull));
    EXPECT_EQ(received_message, 0x8123u);
    EXPECT_EQ(received_wparam, 0x1122334455667788ull);
    EXPECT_EQ(received_lparam, 0x8877665544332211ull);
}

// PostMessage to a window a peer owns (Learn: an HWND is valid system-wide):
// the full-width WPARAM/LPARAM reach the owner, and a window the owner does
// not know fails the post (the sender reports ERROR_INVALID_WINDOW_HANDLE).
TEST(RemoteMemory, PostsWindowMessageToPeerEndpoint) {
    GuestMemory memory;
    winapi::VirtualMemoryManager virtual_memory(memory);
    ipc::RemoteMemoryServer server(memory, virtual_memory);

    u32 received_hwnd = 0;
    u32 received_message = 0;
    u64 received_wparam = 0;
    u64 received_lparam = 0;
    server.set_window_post_message_handler(
        [&](u32 hwnd, u32 message, u64 wparam, u64 lparam) {
            if (hwnd != 0x10042u) return false;
            received_hwnd = hwnd;
            received_message = message;
            received_wparam = wparam;
            received_lparam = lparam;
            return true;
        });
    ASSERT_TRUE(server.start(false));

    ASSERT_TRUE(ipc::remote_post_message(::getpid(), 0x10042u, 0xC123u,
                                         0x1122334455667788ull,
                                         0x8877665544332211ull));
    EXPECT_EQ(received_hwnd, 0x10042u);
    EXPECT_EQ(received_message, 0xC123u);
    EXPECT_EQ(received_wparam, 0x1122334455667788ull);
    EXPECT_EQ(received_lparam, 0x8877665544332211ull);
    EXPECT_FALSE(ipc::remote_post_message(::getpid(), 0x10044u, 0xC123u, 0, 0));
    EXPECT_FALSE(ipc::remote_post_message(::getpid(), 0, 0xC123u, 0, 0));
}

} // namespace
