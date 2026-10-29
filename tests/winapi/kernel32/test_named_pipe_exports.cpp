/// @file test_named_pipe_exports.cpp
/// @brief CreateNamedPipe A/W registration and named-pipe last-error contract.
///
/// Every documented Win32 failure path leaves a retrievable error code behind.
/// A create that answers INVALID_HANDLE_VALUE while GetLastError() still reads
/// 0 is not a state Windows can produce, so a guest that logs "failed (GLE 0)"
/// is reporting a Vitreo contract hole, not its own bug.
///
/// CreateNamedPipeA was never registered, so ANSI callers reached the generic
/// missing-export stub: it returns 0xFFFFFFFF — which *is* INVALID_HANDLE_VALUE
/// — and never touches the last-error slot. Same landmine as CopyFileA in
/// test_copyfile_exports.cpp.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include <capsule/vfs/vfs.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/system/ntdll/ntdll_ext.hpp>

#include <algorithm>
#include <filesystem>
#include <future>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

[[nodiscard]] const ExportEntry* find_export(const DllExports& e,
                                             std::string_view name) {
    auto it = std::find_if(e.entries.begin(), e.entries.end(),
                           [&](const ExportEntry& x) { return x.name == name; });
    return (it == e.entries.end()) ? nullptr : &*it;
}

constexpr GuestAddr kRegion = 0x40000;
constexpr GuestAddr kStack = kRegion + 0x800;
constexpr GuestAddr kNameAddr = kRegion + 0x100;

class NamedPipeExportTest : public ::testing::Test {
protected:
    GuestMemory memory;
    capsule::Vfs vfs;
    HandleTable handles;
    FileManager file_mgr{vfs, handles, memory};
    Kernel32ExtManager mgr{memory, vfs};
    DllExports exports;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kRegion, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        mgr.set_file_manager(&file_mgr);
        exports = create_kernel32_ext_thunks(mgr);
    }

    void write_ansi(GuestAddr addr, std::string_view text) {
        for (usize i = 0; i < text.size(); ++i) {
            ASSERT_TRUE(memory.write_u8(addr + static_cast<GuestAddr>(i),
                                        static_cast<u8>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u8(
            addr + static_cast<GuestAddr>(text.size()), 0).is_ok());
    }

    void write_wide(GuestAddr addr, std::string_view text) {
        for (usize i = 0; i < text.size(); ++i) {
            ASSERT_TRUE(memory.write_u16(addr + static_cast<GuestAddr>(i * 2),
                                         static_cast<u16>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u16(
            addr + static_cast<GuestAddr>(text.size() * 2), 0).is_ok());
    }

    /// 32-bit stdcall frame: return address at esp, arguments from esp+4.
    u32 call32(const ExportEntry& entry, const std::vector<u32>& args) {
        cpu::CpuState cpu;
        cpu.init_protected_mode();
        cpu.set_sp(kStack);
        EXPECT_TRUE(memory.write_u32(kStack, 0xDEADBEEF).is_ok());
        for (usize i = 0; i < args.size(); ++i) {
            EXPECT_TRUE(memory.write_u32(
                kStack + 4 + static_cast<GuestAddr>(i * 4), args[i]).is_ok());
        }
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        entry.handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }

    /// CreateNamedPipe argument frame with a byte-mode, blocking, 1-instance
    /// pipe and default buffer sizes.
    std::vector<u32> create_args(GuestAddr name) const {
        return {static_cast<u32>(name), /*dwOpenMode=*/3u, /*dwPipeMode=*/0u,
                /*nMaxInstances=*/1u, /*nOutBufferSize=*/0u,
                /*nInBufferSize=*/0u, /*nDefaultTimeOut=*/0u,
                /*lpSecurityAttributes=*/0u};
    }

    std::string unique_pipe_name() const {
        return "\\\\.\\pipe\\vitreo_named_pipe_test_" +
               std::to_string(::getpid());
    }
};

TEST_F(NamedPipeExportTest, BothAnsiAndWideCreateFormsAreRegistered) {
    const auto* a = find_export(exports, "CreateNamedPipeA");
    const auto* w = find_export(exports, "CreateNamedPipeW");
    ASSERT_NE(a, nullptr) << "CreateNamedPipeA missing → ANSI callers hit the "
                             "missing-export stub, which returns "
                             "INVALID_HANDLE_VALUE with GetLastError() == 0";
    ASSERT_NE(w, nullptr);
    // 8 args * 4 bytes.
    EXPECT_EQ(a->stdcall_bytes, 32u);
    EXPECT_EQ(w->stdcall_bytes, 32u);
}

TEST_F(NamedPipeExportTest, AnsiCreateBuildsAServerAndClearsLastError) {
    const auto* a = find_export(exports, "CreateNamedPipeA");
    ASSERT_NE(a, nullptr);
    write_ansi(kNameAddr, unique_pipe_name() + "_ansi");
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);

    const u32 result = call32(*a, create_args(kNameAddr));

    EXPECT_NE(result, static_cast<u32>(INVALID_HANDLE));
    EXPECT_NE(result, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    EXPECT_EQ(handles.get_type(static_cast<Handle>(result)), HandleType::Pipe);
    (void)file_mgr.close_file(static_cast<Handle>(result));
}

TEST_F(NamedPipeExportTest, WideCreateStillBuildsAServer) {
    const auto* w = find_export(exports, "CreateNamedPipeW");
    ASSERT_NE(w, nullptr);
    write_wide(kNameAddr, unique_pipe_name() + "_wide");

    const u32 result = call32(*w, create_args(kNameAddr));

    EXPECT_NE(result, static_cast<u32>(INVALID_HANDLE));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    (void)file_mgr.close_file(static_cast<Handle>(result));
}

TEST_F(NamedPipeExportTest, ExtendedPrefixCreatesAndOpensTheSameLocalPipe) {
    const auto* a = find_export(exports, "CreateNamedPipeA");
    ASSERT_NE(a, nullptr);
    for (bool extended_server : {false, true}) {
        const std::string device_name = unique_pipe_name() +
            (extended_server ? "_extended_server" : "_extended_client");
        std::string extended_name = device_name;
        extended_name[2] = '?';
        write_ansi(kNameAddr, extended_server ? extended_name : device_name);
        const Handle server = call32(*a, create_args(kNameAddr));
        ASSERT_NE(server, INVALID_HANDLE);
        EXPECT_TRUE(file_mgr.wait_named_pipe(device_name, 0));
        const Handle client = file_mgr.create_file(
            extended_server ? device_name : extended_name, 0xC0000000u, 0, 3, 0);
        EXPECT_NE(client, INVALID_HANDLE);
        EXPECT_EQ(handles.get_type(client), HandleType::Pipe);
        if (client != INVALID_HANDLE) (void)file_mgr.close_file(client);
        (void)file_mgr.close_file(server);
    }
}

TEST_F(NamedPipeExportTest, NonPipeNameFailsWithInvalidName) {
    // Learn (CreateNamedPipeW): lpName must be \\.\pipe\pipename.
    const auto* a = find_export(exports, "CreateNamedPipeA");
    ASSERT_NE(a, nullptr);
    write_ansi(kNameAddr, "C:\\not\\a\\pipe");
    set_last_error(error_codes::ERROR_SUCCESS);

    const u32 result = call32(*a, create_args(kNameAddr));

    EXPECT_EQ(result, static_cast<u32>(INVALID_HANDLE));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_NAME);
}

TEST_F(NamedPipeExportTest, RejectedPipeModeNeverLeavesLastErrorZero) {
    const auto* w = find_export(exports, "CreateNamedPipeW");
    ASSERT_NE(w, nullptr);
    write_wide(kNameAddr, unique_pipe_name() + "_mode");

    // nMaxInstances == 0 is out of the documented 1..255 range.
    auto args = create_args(kNameAddr);
    args[3] = 0u;
    set_last_error(error_codes::ERROR_SUCCESS);
    EXPECT_EQ(call32(*w, args), static_cast<u32>(INVALID_HANDLE));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    // PIPE_READMODE_MESSAGE on a byte-type pipe is invalid.
    args = create_args(kNameAddr);
    args[2] = 0x00000002u;  // PIPE_READMODE_MESSAGE without PIPE_TYPE_MESSAGE
    set_last_error(error_codes::ERROR_SUCCESS);
    EXPECT_EQ(call32(*w, args), static_cast<u32>(INVALID_HANDLE));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(NamedPipeExportTest, UnreadableNamePointerReportsAccessError) {
    const auto* w = find_export(exports, "CreateNamedPipeW");
    ASSERT_NE(w, nullptr);
    set_last_error(error_codes::ERROR_SUCCESS);

    // An unmapped lpName: Windows dereferences it, so the answer is an access
    // error — never success, and never a zero last-error with a failed create.
    const u32 result = call32(*w, create_args(0x7FFF0000u));

    EXPECT_EQ(result, static_cast<u32>(INVALID_HANDLE));
    EXPECT_NE(get_last_error(), error_codes::ERROR_SUCCESS);
}

// A client CreateFile on a pipe with no server must report the documented
// absence, not silence.
TEST_F(NamedPipeExportTest, ClientOpenOfAbsentPipeSetsFileNotFound) {
    set_last_error(error_codes::ERROR_SUCCESS);
    const Handle h = file_mgr.create_file(
        "\\\\.\\pipe\\vitreo_absent_pipe_" + std::to_string(::getpid()),
        /*GENERIC_READ|GENERIC_WRITE=*/0xC0000000u, 0, /*OPEN_EXISTING=*/3, 0);
    EXPECT_EQ(h, INVALID_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}

TEST_F(NamedPipeExportTest, NativeModeQueryTracksOpenFlagsAndInheritedEndpoints) {
    NtdllExtManager nt;
    nt.set_vfs(&vfs);
    nt.set_handle_table(&handles);
    nt.set_file_manager(&file_mgr);
    const auto* create = find_export(exports, "CreateNamedPipeA");
    ASSERT_NE(create, nullptr);
    for (bool overlapped : {false, true}) {
        const std::string name = "\\\\.\\pipe\\vitreo_mode_" +
            std::to_string(::getpid()) + (overlapped ? "_async" : "_sync");
        write_ansi(kNameAddr, name);
        auto args = create_args(kNameAddr);
        args[1] |= overlapped ? 0x40000000u : 0;
        const Handle server = call32(*create, args);
        ASSERT_NE(server, INVALID_HANDLE);
        // The client's mode is independent of the server's mode.
        const Handle client = file_mgr.create_file(name, 0xC0000000u, 0, 3,
                                                   overlapped ? 0 : 0x40000000u);
        ASSERT_NE(client, INVALID_HANDLE);
        const auto check_mode = [&](Handle h, u32 expected) {
            u32 mode = ~0u, written = 0;
            EXPECT_EQ(nt.nt_query_information_file(h, 16,
                reinterpret_cast<u8*>(&mode), 4, &written), 0u);
            EXPECT_EQ(mode, expected);
            EXPECT_EQ(written, 4u);
            EXPECT_NE(nt.nt_query_information_file(h, 16,
                reinterpret_cast<u8*>(&mode), 3, &written), 0u);
            EXPECT_EQ(written, 0u);
        };
        check_mode(server, overlapped ? 0 : 0x20);
        check_mode(client, overlapped ? 0x20 : 0);
        const Handle duplicate = file_mgr.duplicate_handle(
            client, 0, handles.get_access(client));   // DUPLICATE_SAME_ACCESS
        ASSERT_NE(duplicate, NULL_HANDLE);
        check_mode(duplicate, overlapped ? 0x20 : 0);
        int fd = -1;
        const auto transfer = file_mgr.prepare_transferred_handle(
            server, HandleAccess::All, false, fd);
        ASSERT_TRUE(transfer.has_value());
        const Handle imported = file_mgr.import_transferred_handle(*transfer, fd);
        ASSERT_NE(imported, NULL_HANDLE);
        check_mode(imported, overlapped ? 0 : 0x20);
        for (Handle h : {imported, duplicate, client, server}) {
            EXPECT_TRUE(handles.close(h));
        }
    }
}

TEST_F(NamedPipeExportTest, ZeroByteReadWaitsForDataWithoutConsumingIt) {
    handles.set_close_callback([&](Handle h, HandleType type, void* object) {
        return file_mgr.on_handle_close(h, type, object);
    });
    const std::string name = "\\\\.\\pipe\\vitreo_zero_read_" + std::to_string(::getpid());
    NamedPipeConfig config;
    config.overlapped = true;
    const Handle server = file_mgr.create_named_pipe_server(name, false, config);
    ASSERT_NE(server, INVALID_HANDLE);
    const Handle client = file_mgr.create_file(name, 0xC0000000u, 0, 3, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    (void)file_mgr.connect_named_pipe(server, 1000);
    std::promise<u32> completed;
    auto completion = completed.get_future();
    file_mgr.set_overlapped_completion_callback(
        [&](Handle, u32 count, GuestAddr, bool, u32, u32 error, bool) {
            EXPECT_EQ(count, 0u);
            completed.set_value(error);
        });
    u32 count = 0;
    constexpr GuestAddr overlapped = kRegion + 0x1000;
    EXPECT_EQ(file_mgr.begin_overlapped_pipe_read(server, 0, 0, overlapped, true, count),
              OverlappedPipeIoResult::Pending);
    EXPECT_EQ(completion.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    ASSERT_TRUE(file_mgr.write_handle_bytes(client, "x"));
    ASSERT_EQ(completion.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(completion.get(), 0u);
    ASSERT_TRUE(file_mgr.read_file(server, kNameAddr, 1, 0, &count));
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(memory.read_u8(kNameAddr).value_or(0), 'x');
    std::promise<u32> disconnected;
    auto disconnect = disconnected.get_future();
    file_mgr.set_overlapped_completion_callback(
        [&](Handle, u32, GuestAddr, bool, u32, u32 error, bool) {
            disconnected.set_value(error);
        });
    EXPECT_EQ(file_mgr.begin_overlapped_pipe_read(server, 0, 0, overlapped, true, count),
              OverlappedPipeIoResult::Pending);
    EXPECT_TRUE(file_mgr.close_file(client));
    const auto disconnect_status = disconnect.wait_for(std::chrono::seconds(2));
    file_mgr.shutdown_overlapped_pipe_io();
    file_mgr.clear_overlapped_completion_callback();
    ASSERT_EQ(disconnect_status, std::future_status::ready);
    EXPECT_EQ(disconnect.get(), error_codes::ERROR_BROKEN_PIPE);
    EXPECT_TRUE(file_mgr.close_file(server));
}

TEST_F(NamedPipeExportTest, ClosingHandleCancelsReadThatRetainsEndpoint) {
    handles.set_close_callback([&](Handle h, HandleType type, void* object) {
        return file_mgr.on_handle_close(h, type, object);
    });
    const std::string name = "\\\\.\\pipe\\vitreo_close_read_" + std::to_string(::getpid());
    NamedPipeConfig config;
    config.overlapped = true;
    const Handle server = file_mgr.create_named_pipe_server(name, false, config);
    ASSERT_NE(server, INVALID_HANDLE);
    const Handle client = file_mgr.create_file(name, 0xC0000000u, 0, 3, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    (void)file_mgr.connect_named_pipe(server, 1000);
    std::promise<u32> completed;
    auto completion = completed.get_future();
    file_mgr.set_overlapped_completion_callback(
        [&](Handle, u32, GuestAddr, bool, u32, u32 error, bool) {
            completed.set_value(error);
        });
    u32 count = 0;
    EXPECT_EQ(file_mgr.begin_overlapped_pipe_read(
                  server, 0, 0, kRegion + 0x1000, true, count),
              OverlappedPipeIoResult::Pending);
    EXPECT_TRUE(file_mgr.close_file(server));
    const auto status = completion.wait_for(std::chrono::seconds(2));
    file_mgr.shutdown_overlapped_pipe_io();
    file_mgr.clear_overlapped_completion_callback();
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(completion.get(), error_codes::ERROR_OPERATION_ABORTED);
    EXPECT_TRUE(file_mgr.close_file(client));
}

TEST_F(NamedPipeExportTest, ConsoleTransferKeepsNonInheritableConnectedEndpoint) {
    handles.set_close_callback([&](Handle h, HandleType type, void* object) {
        return file_mgr.on_handle_close(h, type, object);
    });
    const std::string name = "\\\\.\\pipe\\vitreo_console_transfer_" + std::to_string(::getpid());
    const Handle server = file_mgr.create_named_pipe_server(name, false);
    ASSERT_NE(server, INVALID_HANDLE);
    const Handle client = file_mgr.create_file(name, 0xC0000000u, 0, 3, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    (void)file_mgr.connect_named_pipe(server, 1000);
    const std::vector<Handle> selected{server};
    EXPECT_TRUE(file_mgr.prepare_inherited_named_pipes(selected).empty());
    std::string descriptor = file_mgr.prepare_inherited_named_pipes(selected, false);
    ASSERT_FALSE(descriptor.empty());
    ASSERT_EQ(descriptor.front(), 'A'); // Transfer the connection, not the listener.
    EXPECT_EQ(handles.get_flags(server) & HandleFlags::Inherit, 0u);

    // A real child receives a separate descriptor table across exec. Duplicate
    // just the transport FD to model that ownership in this in-process test.
    const auto first = descriptor.find(',');
    const auto second = descriptor.find(',', first + 1);
    const auto third = descriptor.find(',', second + 1);
    const int transferred_fd = ::dup(std::stoi(descriptor.substr(second + 1, third - second - 1)));
    ASSERT_GE(transferred_fd, 0);
    descriptor.replace(second + 1, third - second - 1, std::to_string(transferred_fd));
    HandleTable child_handles;
    FileManager child_files(vfs, child_handles, memory);
    child_handles.set_close_callback([&](Handle h, HandleType type, void* object) {
        return child_files.on_handle_close(h, type, object);
    });
    ASSERT_TRUE(child_files.import_inherited_named_pipes(descriptor));
    EXPECT_TRUE(file_mgr.close_file(server));
    ASSERT_TRUE(child_files.write_handle_bytes(server, "progress 42%"));
    u32 count = 0;
    ASSERT_TRUE(file_mgr.read_file(client, kNameAddr, 12, 0, &count));
    EXPECT_EQ(count, 12u);
    EXPECT_EQ(memory.read_u8(kNameAddr).value_or(0), 'p');
    EXPECT_TRUE(child_files.close_file(server));
    child_handles.set_close_callback({});
    EXPECT_TRUE(file_mgr.close_file(client));
}

// A console-only transfer clears FD_CLOEXEC on an endpoint that is NOT
// inheritable. Windows never hands a non-inheritable handle to a later child,
// so after the spawn the transport must be close-on-exec again; otherwise every
// later child keeps a copy and the peer never sees this end close.
TEST_F(NamedPipeExportTest, ConsoleTransferRestoresCloseOnExecAfterSpawn) {
    handles.set_close_callback([&](Handle h, HandleType type, void* object) {
        return file_mgr.on_handle_close(h, type, object);
    });
    const std::string name = "\\\\.\\pipe\\vitreo_console_cloexec_" + std::to_string(::getpid());
    const Handle server = file_mgr.create_named_pipe_server(name, false);
    ASSERT_NE(server, INVALID_HANDLE);
    const Handle client = file_mgr.create_file(name, 0xC0000000u, 0, 3, 0);
    ASSERT_NE(client, INVALID_HANDLE);
    (void)file_mgr.connect_named_pipe(server, 1000);
    ASSERT_EQ(handles.get_flags(server) & HandleFlags::Inherit, 0u);

    const std::vector<Handle> selected{server};
    const std::string descriptor =
        file_mgr.prepare_inherited_named_pipes(selected, /*require_inheritable=*/false);
    ASSERT_FALSE(descriptor.empty());
    const auto first = descriptor.find(',');
    const auto second = descriptor.find(',', first + 1);
    const auto third = descriptor.find(',', second + 1);
    const int fd = std::stoi(descriptor.substr(second + 1, third - second - 1));
    ASSERT_EQ(::fcntl(fd, F_GETFD) & FD_CLOEXEC, 0) << "prepared for the spawn";

    file_mgr.restore_inherited_pipe_close_on_exec();
    EXPECT_NE(::fcntl(fd, F_GETFD) & FD_CLOEXEC, 0);

    EXPECT_TRUE(file_mgr.close_file(server));
    EXPECT_TRUE(file_mgr.close_file(client));
}

}  // namespace

TEST_F(NamedPipeExportTest, NullDeviceSupportsIoAndInheritedCharacterHandle) {
    // NUL is a device, not a file on a drive, but Vitreo adopts its descriptor
    // through the VFS and Vfs::adopt_native_fd scopes an adopted descriptor to
    // a mounted drive. Every real capsule has C: mounted; this fixture does
    // not, so mount one rather than let the adoption fail with
    // DriveNotMounted. See the comment on adopt_native_fd for why that
    // scoping is questionable for descriptors that have no path at all.
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("vitreo_nul_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    ASSERT_TRUE(vfs.mount_drive(
                    'C', std::make_unique<capsule::HostFileSystem>(
                             root.string()))
                    .is_ok());

    handles.set_close_callback([&](Handle h, HandleType type, void* object) {
        return file_mgr.on_handle_close(h, type, object);
    });
    const auto file_exports = make_file_exports(file_mgr);
    const auto* get_type = find_export(file_exports, "GetFileType");
    ASSERT_NE(get_type, nullptr);
    for (const auto name : {"NUL", "nul.txt", "\\\\.\\NUL"}) {
        const Handle h = file_mgr.create_file(name, 0xC0000000u, 3, 3, 0);
        ASSERT_NE(h, INVALID_HANDLE) << name;
        EXPECT_EQ(call32(*get_type, {h}), 2u);
        write_ansi(kRegion + 0x400, "discard");
        u32 count = 0;
        ASSERT_TRUE(file_mgr.write_file(h, kRegion + 0x400, 7, 0, &count));
        EXPECT_EQ(count, 7u);
        count = 99;
        ASSERT_TRUE(file_mgr.read_file(h, kRegion + 0x400, 7, 0, &count));
        EXPECT_EQ(count, 0u);
        EXPECT_EQ(memory.read_u8(kRegion + 0x400).value_or(0), 'd');
        EXPECT_TRUE(file_mgr.close_file(h));
    }
    const Handle h = file_mgr.create_file("NUL", 0xC0000000u, 3, 3, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    handles.set_flags(h, HandleFlags::Inherit, HandleFlags::Inherit);
    auto descriptor = file_mgr.prepare_inherited_named_pipes(std::vector<Handle>{h});
    ASSERT_FALSE(descriptor.empty());
    EXPECT_EQ(descriptor.front(), 'F');
    const auto a = descriptor.find(',', 2);
    const auto b = descriptor.find(',', a + 1);
    const int fd = std::stoi(descriptor.substr(a + 1, b - a - 1));
    const int child_fd = ::dup(fd);
    ASSERT_GE(child_fd, 0);
    ::close(fd); // F descriptors own a prepared duplicate.
    descriptor.replace(a + 1, b - a - 1, std::to_string(child_fd));
    HandleTable child_handles;
    FileManager child(vfs, child_handles, memory);
    child_handles.set_close_callback([&](Handle n, HandleType t, void* o) {
        return child.on_handle_close(n, t, o);
    });
    ASSERT_TRUE(child.import_inherited_named_pipes(descriptor));
    EXPECT_TRUE(file_mgr.close_file(h));
    EXPECT_TRUE(child.is_character_file(h));
    u32 count = 99;
    EXPECT_TRUE(child.read_file(h, kRegion + 0x400, 7, 0, &count));
    EXPECT_EQ(count, 0u);
    EXPECT_TRUE(child.close_file(h));
    child_handles.set_close_callback({});
}

// FileAccessInformation reports the handle's granted access with GENERIC_*
// mapped through FILE_GENERIC_* (tools/oracle_probe/file_access_info_probe,
// x86 = x64). libuv's uv_pipe_open derives readable/writable from
// FILE_READ_DATA/FILE_WRITE_DATA here: an unmapped GENERIC_READ made every
// read of Electron's child stdio pipes fail with ENOTCONN.
TEST_F(NamedPipeExportTest, FileAccessInformationReportsMappedGrantedAccess) {
    NtdllExtManager nt;
    nt.set_vfs(&vfs);
    nt.set_handle_table(&handles);
    nt.set_file_manager(&file_mgr);
    const auto access_of = [&](Handle h) {
        u32 access = 0xDEADBEEFu, written = 0;
        EXPECT_EQ(nt.nt_query_information_file(h, 8 /*FileAccessInformation*/,
                                               reinterpret_cast<u8*>(&access), 4, &written),
                  0u);
        EXPECT_EQ(written, 4u);
        return access;
    };
    Handle read_end = NULL_HANDLE, write_end = NULL_HANDLE;
    ASSERT_TRUE(file_mgr.create_anonymous_pipe(read_end, write_end, true));
    EXPECT_EQ(access_of(read_end), 0x00120189u);
    EXPECT_EQ(access_of(write_end), 0x00120196u);
    // DUPLICATE_SAME_ACCESS keeps the mask; asking for GENERIC_READ gives
    // the plain mapping; a right the source lacks is refused.
    const Handle same = file_mgr.duplicate_handle(read_end, 0, handles.get_access(read_end));
    ASSERT_NE(same, NULL_HANDLE);
    EXPECT_EQ(access_of(same), 0x00120189u);
    const Handle generic = file_mgr.duplicate_handle(read_end, 0, 0x80000000u);
    ASSERT_NE(generic, NULL_HANDLE);
    EXPECT_EQ(access_of(generic), 0x00120089u);
    const Handle specific = file_mgr.duplicate_handle(read_end, 0, 0x1u /*FILE_READ_DATA*/);
    EXPECT_NE(specific, NULL_HANDLE);
    EXPECT_EQ(file_mgr.duplicate_handle(read_end, 0, 0x40000000u), NULL_HANDLE);
    for (Handle h : {generic, specific}) EXPECT_TRUE(handles.close(h));

    const std::string name = "\\\\.\\pipe\\vitreo_access_" + std::to_string(::getpid());
    write_ansi(kNameAddr, name);
    const auto* create = find_export(exports, "CreateNamedPipeA");
    ASSERT_NE(create, nullptr);
    const Handle server = call32(*create, create_args(kNameAddr));
    ASSERT_NE(server, INVALID_HANDLE);
    const Handle duplex_client = file_mgr.create_file(name, 0xC0000000u, 0, 3, 0);
    ASSERT_NE(duplex_client, INVALID_HANDLE);
    EXPECT_EQ(access_of(duplex_client), 0x0012019Fu);
    for (Handle h : {duplex_client, server, same, read_end, write_end}) {
        EXPECT_TRUE(handles.close(h));
    }
}
