// SPDX-License-Identifier: LGPL-2.1-only

#include "dwrite_manager_harness.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

#include "capsule/vfs/host_filesystem.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::dwrite_test;

TEST(DWriteLocalFontStreamTest, CreateStreamFromKeyExposesMappedFragments) {
    GuestMemory memory;
    cpu::CpuState cpu;
    ASSERT_TRUE(memory.map_region(0x1000, 0x2000, MemoryProtection::ReadWrite)
                    .is_ok());
    ASSERT_TRUE(memory.map_region(0x80000, 0x1000, MemoryProtection::ReadWrite)
                    .is_ok());
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = 0x80800;

    char temp_template[] = "/tmp/vitreo_dwrite_stream_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    const std::string test_root = temp_dir;
    const auto cleanup = [&] {
        std::error_code ec;
        std::filesystem::remove_all(test_root, ec);
    };
    struct CleanupGuard {
        decltype(cleanup) fn;
        ~CleanupGuard() { fn(); }
    } guard{cleanup};

    constexpr std::string_view payload = "DWriteStreamPayload!";
    {
        std::ofstream out(test_root + "/sample.font", std::ios::binary);
        ASSERT_TRUE(out.good());
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ASSERT_TRUE(out.good());
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
                    'C', std::make_unique<capsule::HostFileSystem>(test_root))
                    .is_ok());

    DWriteManager manager{memory, true, &vfs, nullptr};
    manager.set_method_thunks(DWriteManagerTest::method_thunks());
    // Per-stream mapping of VITREO_DWRITE_FONT_REUSE=0; the shared mapping
    // is covered by ChangedLocalFileIsNotServedStale.
    manager.set_font_reuse_enabled(false);

    constexpr GuestAddr IID_ADDRESS = 0x1100;
    constexpr GuestAddr OUTPUT = 0x1200;
    constexpr GuestAddr PATH_ADDRESS = 0x1300;
    constexpr GuestAddr KEY_OUT = 0x1400;
    constexpr GuestAddr KEY_SIZE_OUT = 0x1408;
    constexpr GuestAddr LOADER_OUT = 0x1410;
    constexpr GuestAddr STREAM_OUT = 0x1418;
    constexpr GuestAddr SIZE_OUT = 0x1420;
    constexpr GuestAddr FRAGMENT_OUT = 0x1428;
    constexpr GuestAddr CONTEXT_OUT = 0x1430;
    constexpr GuestAddr TIME_OUT = 0x1438;

    const Guid factory_iid{
        0xB859EE5Au, 0xD838u, 0x4B5Bu,
        {0xA2, 0xE8, 0x1A, 0xDC, 0x7D, 0x93, 0xDB, 0x48}};
    ASSERT_TRUE(memory.write_bytes(
                    IID_ADDRESS, reinterpret_cast<const Byte*>(&factory_iid),
                    sizeof(factory_iid))
                    .is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT;
    manager.impl_DWriteCreateFactory(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr factory = memory.read_u64(OUTPUT).value();
    ASSERT_NE(factory, 0u);

    const std::u16string path = u"C:\\sample.font";
    ASSERT_TRUE(memory.write_bytes(
                    PATH_ADDRESS,
                    reinterpret_cast<const Byte*>(path.c_str()),
                    (path.size() + 1) * sizeof(char16_t))
                    .is_ok());

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = PATH_ADDRESS;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = OUTPUT;
    manager.impl_CreateFontFileReference(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr file = memory.read_u64(OUTPUT).value();
    ASSERT_NE(file, 0u);

    cpu.gpr.rcx = file;
    cpu.gpr.rdx = KEY_OUT;
    cpu.gpr.r8 = KEY_SIZE_OUT;
    manager.impl_FileGetReferenceKey(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr key = memory.read_u64(KEY_OUT).value();
    const u32 key_size = memory.read_u32(KEY_SIZE_OUT).value();
    ASSERT_NE(key, 0u);
    ASSERT_GT(key_size, 0u);

    cpu.gpr.rcx = file;
    cpu.gpr.rdx = LOADER_OUT;
    manager.impl_FileGetLoader(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr loader = memory.read_u64(LOADER_OUT).value();
    ASSERT_NE(loader, 0u);
    const GuestAddr loader_vt = memory.read_u64(loader).value();
    EXPECT_EQ(memory.read_u64(loader_vt + 3 * 8).value(),
              DWriteManagerTest::LOCAL_CREATE_STREAM_THUNK);
    EXPECT_EQ(memory.read_u64(loader_vt + 4 * 8).value(),
              DWriteManagerTest::LOCAL_GET_PATH_LENGTH_THUNK);

    cpu.gpr.rcx = loader;
    cpu.gpr.rdx = key;
    cpu.gpr.r8 = key_size;
    cpu.gpr.r9 = STREAM_OUT;
    manager.impl_LocalLoaderCreateStreamFromKey(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr stream = memory.read_u64(STREAM_OUT).value();
    ASSERT_NE(stream, 0u);
    const GuestAddr stream_vt = memory.read_u64(stream).value();
    EXPECT_EQ(memory.read_u64(stream_vt + 3 * 8).value(),
              DWriteManagerTest::STREAM_READ_FRAGMENT_THUNK);
    EXPECT_EQ(memory.read_u64(stream_vt + 5 * 8).value(),
              DWriteManagerTest::STREAM_GET_FILE_SIZE_THUNK);

    cpu.gpr.rcx = stream;
    cpu.gpr.rdx = SIZE_OUT;
    manager.impl_StreamGetFileSize(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(SIZE_OUT).value(), payload.size());

    cpu.gpr.rcx = stream;
    cpu.gpr.rdx = TIME_OUT;
    manager.impl_StreamGetLastWriteTime(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = stream;
    cpu.gpr.rdx = FRAGMENT_OUT;
    cpu.gpr.r8 = 6; // fileOffset into "DWriteStreamPayload!"
    cpu.gpr.r9 = 4; // fragmentSize "Stre"
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, CONTEXT_OUT).is_ok());
    manager.impl_StreamReadFileFragment(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr fragment = memory.read_u64(FRAGMENT_OUT).value();
    ASSERT_NE(fragment, 0u);
    std::array<char, 4> bytes{};
    ASSERT_TRUE(memory.read_bytes(
                    fragment, reinterpret_cast<Byte*>(bytes.data()), 4)
                    .is_ok());
    EXPECT_EQ(std::string_view(bytes.data(), 4), "Stre");

    cpu.gpr.rcx = stream;
    cpu.gpr.rdx = FRAGMENT_OUT;
    cpu.gpr.r8 = payload.size();
    cpu.gpr.r9 = 1;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, CONTEXT_OUT).is_ok());
    manager.impl_StreamReadFileFragment(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80070057u);

    cpu.gpr.rcx = stream;
    cpu.gpr.rdx = 0;
    manager.impl_StreamReleaseFileFragment(marshaler);

    const GuestAddr mapped = fragment - 6;
    cpu.gpr.rcx = stream;
    manager.impl_Release(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_FALSE(memory.is_mapped(mapped));
}

TEST_F(DWriteManagerTest, RepeatedFaceFilesKeepIdentityAndRepublishMissingFile) {
    char pattern[] = "/tmp/vitreo-font-publication-XXXXXX";
    char* dir = ::mkdtemp(pattern);
    ASSERT_NE(dir, nullptr);
    const std::filesystem::path root(dir);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{root};
    std::filesystem::create_directories(root / "Windows/Fonts");
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())));
    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);
    Marshaler marshaler(memory); marshaler.attach_cpu(cpu);
    constexpr GuestAddr count = DATA_BASE + 0x600;
    constexpr GuestAddr files = DATA_BASE + 0x608;
    constexpr GuestAddr key_out = DATA_BASE + 0x610;
    constexpr GuestAddr size_out = DATA_BASE + 0x618;
    std::vector<Byte> first_key;
    const auto get_file = [&] {
        EXPECT_TRUE(memory.write_u32(count, 1));
        cpu.gpr.rcx = face; cpu.gpr.rdx = count; cpu.gpr.r8 = files;
        manager.impl_FaceGetFiles(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        if (cpu.gpr.rax != 0) return;
        const GuestAddr file = memory.read_u64(files).value();
        cpu.gpr.rcx = file; cpu.gpr.rdx = key_out; cpu.gpr.r8 = size_out;
        manager.impl_FileGetReferenceKey(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        std::vector<Byte> key(memory.read_u32(size_out).value());
        EXPECT_TRUE(memory.read_bytes(memory.read_u64(key_out).value(), key.data(), key.size()));
        if (first_key.empty()) first_key = key;
        else EXPECT_EQ(key, first_key);
        cpu.gpr.rcx = file; manager.impl_Release(marshaler);
    };
    get_file();
    ASSERT_FALSE(first_key.empty());
    std::vector<std::filesystem::path> published;
    for (const auto& entry : std::filesystem::directory_iterator(root / "Windows/Fonts"))
        published.push_back(entry.path());
    ASSERT_EQ(published.size(), 1u);
    const auto read = [&] {
        std::ifstream input(published[0], std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(input), {});
    };
    const auto original = read();
    ASSERT_FALSE(original.empty());
    for (int i = 0; i < 50; ++i) get_file();
    EXPECT_EQ(read(), original);
    ASSERT_TRUE(std::filesystem::remove(published[0]));
    get_file();
    EXPECT_EQ(read(), original);
    { std::ofstream truncated(published[0], std::ios::binary | std::ios::trunc); truncated << "x"; }
    get_file();
    EXPECT_EQ(read(), original);
}

// IDWriteLocalFontFileLoader streams: a file is the same file while its path,
// size and last write time (GetLastWriteTimeFromKey) are unchanged.
TEST(DWriteLocalFontStreamTest, ChangedLocalFileIsNotServedStale) {
    GuestMemory memory;
    cpu::CpuState cpu;
    ASSERT_TRUE(memory.map_region(0x1000, 0x2000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = 0x80800;
    char temp_template[] = "/tmp/vitreo_dwrite_stale_XXXXXX";
    char* temp_dir = ::mkdtemp(temp_template);
    ASSERT_NE(temp_dir, nullptr);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{temp_dir};
    const std::filesystem::path host_file = std::filesystem::path(temp_dir) / "a.font";
    const auto write_file = [&](std::string_view text, int age_seconds) {
        { std::ofstream out(host_file, std::ios::binary | std::ios::trunc); out << text; }
        std::filesystem::last_write_time(
            host_file, std::filesystem::file_time_type::clock::now() -
                           std::chrono::seconds(age_seconds));
    };
    write_file("VERSION-ONE-PAYLOAD", 100);

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(temp_dir)).is_ok());
    DWriteManager manager{memory, true, &vfs, nullptr};
    manager.set_method_thunks(DWriteManagerTest::method_thunks());
    constexpr GuestAddr IID = 0x1100, OUT = 0x1200, PATH = 0x1300, KEY_OUT = 0x1400,
                        KEY_SIZE_OUT = 0x1408, FRAGMENT_OUT = 0x1428, CONTEXT_OUT = 0x1430,
                        TIME_OUT = 0x1438;
    ASSERT_TRUE(memory.write_bytes(IID, reinterpret_cast<const Byte*>(&IID_IDWRITE_FACTORY),
                                   sizeof(IID_IDWRITE_FACTORY)).is_ok());
    Marshaler m(memory);
    m.attach_cpu(cpu);
    const auto call = [&](void (DWriteManager::*impl)(Marshaler&), u64 a = 0, u64 b = 0,
                          u64 c = 0, u64 d = 0, u64 e = 0) {
        cpu.gpr.rcx = a; cpu.gpr.rdx = b; cpu.gpr.r8 = c; cpu.gpr.r9 = d;
        EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, e).is_ok());
        (manager.*impl)(m);
        return static_cast<u32>(cpu.gpr.rax);
    };
    ASSERT_EQ(call(&DWriteManager::impl_DWriteCreateFactory, 0, IID, OUT), 0u);
    const GuestAddr factory = memory.read_u64(OUT).value();
    const std::u16string path = u"C:\\a.font";
    ASSERT_TRUE(memory.write_bytes(PATH, reinterpret_cast<const Byte*>(path.c_str()),
                                   (path.size() + 1) * sizeof(char16_t)).is_ok());
    ASSERT_EQ(call(&DWriteManager::impl_CreateFontFileReference, factory, PATH, 0, OUT), 0u);
    const GuestAddr file = memory.read_u64(OUT).value();
    ASSERT_EQ(call(&DWriteManager::impl_FileGetReferenceKey, file, KEY_OUT, KEY_SIZE_OUT), 0u);
    const GuestAddr key = memory.read_u64(KEY_OUT).value();
    const u32 key_size = memory.read_u32(KEY_SIZE_OUT).value();
    ASSERT_EQ(call(&DWriteManager::impl_FileGetLoader, file, OUT), 0u);
    const GuestAddr loader = memory.read_u64(OUT).value();

    struct Opened { GuestAddr stream; GuestAddr data; std::string bytes; u64 time; };
    const auto open = [&]() -> Opened {
        EXPECT_EQ(call(&DWriteManager::impl_LocalLoaderCreateStreamFromKey,
                       loader, key, key_size, OUT), 0u);
        const GuestAddr stream = memory.read_u64(OUT).value_or(0);
        EXPECT_EQ(call(&DWriteManager::impl_StreamGetFileSize, stream, OUT), 0u);
        const u64 size = memory.read_u64(OUT).value_or(0);
        EXPECT_EQ(call(&DWriteManager::impl_StreamGetLastWriteTime, stream, TIME_OUT), 0u);
        EXPECT_EQ(call(&DWriteManager::impl_StreamReadFileFragment,
                       stream, FRAGMENT_OUT, 0, size, CONTEXT_OUT), 0u);
        const GuestAddr data = memory.read_u64(FRAGMENT_OUT).value_or(0);
        std::string bytes(size, '\0');
        EXPECT_TRUE(memory.read_bytes(data, reinterpret_cast<Byte*>(bytes.data()), size).is_ok());
        // Out of bounds by one byte must fail (ReadFileFragment remarks).
        EXPECT_EQ(call(&DWriteManager::impl_StreamReadFileFragment,
                       stream, FRAGMENT_OUT, 0, size + 1, CONTEXT_OUT), 0x80070057u);
        return {stream, data, bytes, memory.read_u64(TIME_OUT).value_or(0)};
    };
    const auto local_time = [&] {
        EXPECT_EQ(call(&DWriteManager::impl_LocalLoaderGetLastWriteTime,
                       loader, key, key_size, TIME_OUT), 0u);
        return memory.read_u64(TIME_OUT).value_or(0);
    };

    const Opened first = open();
    EXPECT_EQ(first.bytes, "VERSION-ONE-PAYLOAD");
    EXPECT_EQ(first.time, local_time());
    const Opened second = open();
    EXPECT_EQ(second.bytes, "VERSION-ONE-PAYLOAD");
    EXPECT_EQ(call(&DWriteManager::impl_Release, first.stream), 0u);
    EXPECT_TRUE(memory.is_mapped(second.data, second.bytes.size()));

    write_file("VERSION-TWO-LONGER-PAYLOAD", 50);  // new size and time
    const Opened third = open();
    EXPECT_EQ(third.bytes, "VERSION-TWO-LONGER-PAYLOAD");
    EXPECT_EQ(third.time, local_time());
    EXPECT_NE(third.time, first.time);
    // The earlier stream's resource stays readable until it is released.
    EXPECT_TRUE(memory.is_mapped(second.data, second.bytes.size()));

    write_file("VERSION-3-LONGER-PAYLOAD!!", 10);  // same size, new time
    const Opened fourth = open();
    EXPECT_EQ(fourth.bytes, "VERSION-3-LONGER-PAYLOAD!!");
    EXPECT_EQ(fourth.time, local_time());
    EXPECT_NE(fourth.time, third.time);

    for (const auto* opened : {&second, &third, &fourth})
        EXPECT_EQ(call(&DWriteManager::impl_Release, opened->stream), 0u);
    EXPECT_FALSE(memory.is_mapped(second.data));
    EXPECT_FALSE(memory.is_mapped(third.data));
}
