/// @file test_file_create_semantics.cpp
/// @brief Unit tests for CreateFile append-only handles and the measured per-disposition last-error codes

#include "kernel32_file_fixture.hpp"

// Chromium --enable-logging=handle --log-file=N puts a File write end in
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST. Windows inherits it at the same numeric
// value; Vitreo must serialize F,<handle>,<fd>,... and allocate_at on import.
TEST(AppendOnlyHandles, ConcurrentAppendersDoNotOverwriteEachOther) {
    // Learn (CreateFile): with FILE_APPEND_DATA and not FILE_WRITE_DATA,
    // "write operations will not overwrite existing data" — every write goes to
    // the current end of file and the file pointer is ignored. Chromium relies
    // on this so a browser process and its children can share one log file.
    // Honouring only each handle's own position makes the writers stomp on one
    // another and produces interleaved, corrupted output.
    constexpr u32 FILE_APPEND_DATA = 0x0004;
    constexpr u32 OPEN_ALWAYS = 4;

    char tmpl[] = "/tmp/vitreo_append_only_XXXXXX";
    ASSERT_NE(::mkdtemp(tmpl), nullptr);
    const std::string root = tmpl;
    {
        std::ofstream out(root + "/shared.log");
        out << "seed\n";
    }

    Vfs vfs;
    ASSERT_TRUE(
        vfs.mount_drive('C', std::make_unique<HostFileSystem>(root)).is_ok());
    HandleTable handles;
    GuestMemory memory;
    ASSERT_TRUE(
        memory.map_region(0x10000, 4096, MemoryProtection::ReadWrite).is_ok());
    FileManager fm(vfs, handles, memory);

    const Handle first =
        fm.create_file("C:\\shared.log", FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, 0);
    const Handle second =
        fm.create_file("C:\\shared.log", FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, 0);
    ASSERT_NE(first, INVALID_HANDLE);
    ASSERT_NE(second, INVALID_HANDLE);

    const auto append = [&](Handle handle, std::string_view text) {
        ASSERT_TRUE(memory
                        .write_bytes(0x10000,
                                     reinterpret_cast<const Byte*>(text.data()),
                                     text.size())
                        .is_ok());
        u32 written = 0;
        ASSERT_TRUE(fm.write_file(handle, 0x10000,
                                  static_cast<u32>(text.size()), 0, &written));
        EXPECT_EQ(written, text.size());
    };

    // Interleave the two handles: with per-handle positions the second writer
    // would restart at offset 0 and overwrite what the first one wrote.
    append(first, "aaa\n");
    append(second, "bbb\n");
    append(first, "ccc\n");
    append(second, "ddd\n");

    ASSERT_TRUE(fm.close_file(first));
    ASSERT_TRUE(fm.close_file(second));

    std::ifstream in(root + "/shared.log", std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
    EXPECT_EQ(contents, "seed\naaa\nbbb\nccc\nddd\n");

    std::filesystem::remove_all(root);
}

// tools/oracle_probe/last_error_on_success_probe.expected (Windows 11):
// a successful CreateFile ALWAYS sets the last error -- ERROR_ALREADY_EXISTS
// for CREATE_ALWAYS/OPEN_ALWAYS on an existing file, 0 otherwise -- while a
// successful seek leaves it untouched unless the new position's low half
// collides with INVALID_SET_FILE_POINTER (then NO_ERROR).
TEST(CreateFileLastError, DispositionSetsMeasuredSuccessCode) {
    constexpr u32 CREATE_NEW_ = 1, CREATE_ALWAYS_ = 2, OPEN_EXISTING_ = 3,
                  OPEN_ALWAYS_ = 4, TRUNCATE_EXISTING_ = 5;
    char tmpl[] = "/tmp/vitreo_create_le_XXXXXX";
    ASSERT_NE(::mkdtemp(tmpl), nullptr);
    const std::string root = tmpl;
    Vfs vfs;
    ASSERT_TRUE(
        vfs.mount_drive('C', std::make_unique<HostFileSystem>(root)).is_ok());
    HandleTable handles;
    GuestMemory memory;
    FileManager fm(vfs, handles, memory);
    // CloseHandle releases the VFS handle through the HandleTable close
    // callback RuntimeContext installs. Without it every "closed" share-0
    // handle stays open and the next reopen is a sharing violation.
    handles.set_close_callback(
        [&fm](Handle h, HandleType type, void* object) -> bool {
            return fm.on_handle_close(h, type, object);
        });
    const u32 rw = GENERIC_READ | GENERIC_WRITE;

    const auto open = [&](const char* path, u32 disposition, u32 expected) {
        set_last_error(0xDEADu);
        const Handle h = fm.create_file(path, rw, 0, disposition, 0);
        ASSERT_NE(h, INVALID_HANDLE) << path << " disp=" << disposition;
        EXPECT_EQ(get_last_error(), expected) << path << " disp=" << disposition;
        EXPECT_TRUE(fm.close_file(h));
    };
    open("C:\\a.txt", CREATE_NEW_, error_codes::ERROR_SUCCESS);
    open("C:\\a.txt", CREATE_ALWAYS_, error_codes::ERROR_ALREADY_EXISTS);
    open("C:\\a.txt", OPEN_ALWAYS_, error_codes::ERROR_ALREADY_EXISTS);
    open("C:\\a.txt", OPEN_EXISTING_, error_codes::ERROR_SUCCESS);
    open("C:\\a.txt", TRUNCATE_EXISTING_, error_codes::ERROR_SUCCESS);
    open("C:\\b.txt", OPEN_ALWAYS_, error_codes::ERROR_SUCCESS);
    open("C:\\c.txt", CREATE_ALWAYS_, error_codes::ERROR_SUCCESS);

    std::filesystem::remove_all(root);
}
