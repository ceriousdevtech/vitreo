/// @file test_final_path_by_handle.cpp
/// @brief GetFinalPathNameByHandleW/A against the Windows capture
///        tools/oracle_probe/final_path_probe.expected (x86 == x64).
///
/// The export was a stub that always failed ERROR_INVALID_HANDLE, which broke
/// Python's os.path.realpath (PyInstaller one-file bootstraps then exit with
/// "Module object for pyimod02_importers is NULL!"). Every expected value here
/// is a probe row, adapted only for the rig's volume root: the probe's
/// `C:\oracle\fpn\Sub Dir\...` is `C:\Sub Dir\...` here.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <capsule/vfs/vfs.hpp>
#include <capsule/vfs/host_filesystem.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 GENERIC_READ_ = 0x80000000u;
constexpr u32 FILE_SHARE_ALL = 0x7u;
constexpr u32 OPEN_EXISTING_ = 3;
constexpr u32 FILE_FLAG_BACKUP_SEMANTICS_ = 0x02000000u;
constexpr u32 kSentinel = 0xDEADu;

constexpr GuestAddr kStack = 0x80000;
constexpr GuestAddr kBuf = 0x20000;

struct Result2 {
    u32 ret;
    u32 err;
};

class FinalPathRig : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/vitreo_final_path_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        root = dir;
        std::filesystem::create_directory(root + "/Sub Dir");
        for (const char* name : {"MixedCase.txt", "caf\xC3\xA9.txt", "\xE4\xB8\xAD.txt",
                                 "before.txt"}) {
            std::ofstream(root + "/Sub Dir/" + name) << "x";
        }
        ASSERT_TRUE(memory.map_region(0x1000, 0x100000, MemoryProtection::ReadWrite)
                        .is_ok());
        ASSERT_TRUE(vfs.mount_drive(
                        'C', std::make_unique<capsule::HostFileSystem>(root))
                        .is_ok());
        files = std::make_unique<FileManager>(vfs, handles, memory);
        mgr = std::make_unique<Kernel32ExtManager>(memory, vfs);
        mgr->set_file_manager(files.get());
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    Handle open(std::string_view path, u32 flags = 0) {
        return files->create_file(path, GENERIC_READ_, FILE_SHARE_ALL, OPEN_EXISTING_, flags);
    }

    void fill(usize bytes) {
        for (usize i = 0; i < bytes; ++i) ASSERT_TRUE(memory.write_u8(kBuf + i, 0xCC).is_ok());
    }
    /// Bytes from kBuf that no longer hold the 0xCC fill.
    usize touched(usize limit = 2048) {
        usize n = 0;
        while (n < limit && memory.read_u8(kBuf + n).value_or(0xCC) != 0xCC) ++n;
        return n;
    }
    std::u16string wide_at(usize chars) {
        std::u16string s;
        for (usize i = 0; i < chars; ++i) {
            s.push_back(static_cast<char16_t>(memory.read_u16(kBuf + i * 2).value_or(0)));
        }
        return s;
    }
    std::string ansi_at(usize chars) {
        std::string s;
        for (usize i = 0; i < chars; ++i) {
            s.push_back(static_cast<char>(memory.read_u8(kBuf + i).value_or(0)));
        }
        return s;
    }

    Result2 call(bool ansi, u64 handle, GuestAddr buf, u32 cch, u32 flags, bool x64 = false) {
        cpu::CpuState cpu;
        if (x64) {
            cpu.init_long_mode();
            cpu.gpr.rsp = kStack;
            cpu.gpr.rcx = handle;
            cpu.gpr.rdx = buf;
            cpu.gpr.r8 = cch;
            cpu.gpr.r9 = flags;
        } else {
            cpu.init_protected_mode();
            cpu.set_sp(kStack);
            (void)memory.write_u32(kStack, 0xDEADBEEF);
            (void)memory.write_u32(kStack + 4, static_cast<u32>(handle));
            (void)memory.write_u32(kStack + 8, static_cast<u32>(buf));
            (void)memory.write_u32(kStack + 12, cch);
            (void)memory.write_u32(kStack + 16, flags);
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        set_last_error(kSentinel);
        if (ansi) {
            mgr->impl_GetFinalPathNameByHandleA(m);
        } else {
            mgr->impl_GetFinalPathNameByHandleW(m);
        }
        return {static_cast<u32>(cpu.gpr.rax), get_last_error()};
    }
    Result2 w(u64 h, u32 flags, u32 cch = 1024, bool x64 = false) {
        fill(2 * 1100);
        return call(false, h, kBuf, cch, flags, x64);
    }
    Result2 a(u64 h, u32 flags, u32 cch = 1024) {
        fill(1100);
        return call(true, h, kBuf, cch, flags);
    }

    std::string root;
    GuestMemory memory;
    capsule::Vfs vfs;
    HandleTable handles;
    std::unique_ptr<FileManager> files;
    std::unique_ptr<Kernel32ExtManager> mgr;
};

const std::u16string kDosFile = u"\\\\?\\C:\\Sub Dir\\MixedCase.txt";
const std::u16string kNoneFile = u"\\Sub Dir\\MixedCase.txt";

}  // namespace

TEST(FinalPathExportsTest, AnsiAndWideAreRegisteredWithSixteenStdcallBytes) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);
    for (const char* name : {"GetFinalPathNameByHandleW", "GetFinalPathNameByHandleA"}) {
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                               [&](const ExportEntry& e) { return e.name == name; });
        ASSERT_NE(it, exports.entries.end()) << name;
        EXPECT_EQ(it->stdcall_bytes, 16u) << name;
    }
}

TEST_F(FinalPathRig, FileReportsStoredSpellingInDosAndNoneForms) {
    const Handle f = open("C:\\Sub Dir\\MixedCase.txt");
    ASSERT_NE(f, INVALID_HANDLE);
    // file flags=0: ret=len, err=0 (the DOS form clears it), NUL written.
    for (u32 flags : {0x0u, 0x8u, 0x10u, 0x100u, 0x80000000u}) {
        const auto r = w(f, flags);
        EXPECT_EQ(r.ret, kDosFile.size()) << flags;
        EXPECT_EQ(r.err, 0u) << flags;
        EXPECT_EQ(wide_at(r.ret), kDosFile) << flags;
        EXPECT_EQ(memory.read_u16(kBuf + r.ret * 2).value_or(1), 0u);
        EXPECT_EQ(touched(), (kDosFile.size() + 1) * 2);
    }
    // file flags=4 / 0xc: no drive, last error untouched.
    for (u32 flags : {0x4u, 0xCu}) {
        const auto r = w(f, flags);
        EXPECT_EQ(r.ret, kNoneFile.size());
        EXPECT_EQ(r.err, kSentinel);
        EXPECT_EQ(wide_at(r.ret), kNoneFile);
    }
}

TEST_F(FinalPathRig, NonCanonicalOpensReportTheStoredName) {
    // file_lc / file_rel / file_ext: case, separators and dot segments of the
    // open do not survive; both name forms give the on-disk spelling.
    const Handle lc = open("c:/SUB DIR/./x/../mixedcase.TXT");
    ASSERT_NE(lc, INVALID_HANDLE);
    EXPECT_EQ(w(lc, 0).ret, kDosFile.size());
    EXPECT_EQ(wide_at(kDosFile.size()), kDosFile);
    EXPECT_EQ(w(lc, 8).ret, kDosFile.size());
    EXPECT_EQ(wide_at(kDosFile.size()), kDosFile);

    const Handle ext = open("\\\\?\\C:\\Sub Dir\\MixedCase.txt");
    ASSERT_NE(ext, INVALID_HANDLE);
    EXPECT_EQ(w(ext, 0).ret, kDosFile.size());
    EXPECT_EQ(wide_at(kDosFile.size()), kDosFile);
}

TEST_F(FinalPathRig, DirectoriesHaveNoTrailingSeparatorAndTheRootKeepsOne) {
    const Handle d = open("C:\\sub dir\\", FILE_FLAG_BACKUP_SEMANTICS_);
    ASSERT_NE(d, INVALID_HANDLE);
    const std::u16string dos = u"\\\\?\\C:\\Sub Dir";
    EXPECT_EQ(w(d, 0).ret, dos.size());
    EXPECT_EQ(wide_at(dos.size()), dos);
    EXPECT_EQ(w(d, 4).ret, 8u);
    EXPECT_EQ(wide_at(8), u"\\Sub Dir");

    const Handle r = open("C:\\", FILE_FLAG_BACKUP_SEMANTICS_);
    ASSERT_NE(r, INVALID_HANDLE);
    EXPECT_EQ(w(r, 0).ret, 7u);  // root flags=0: \\?\C:\  (ret 7)
    EXPECT_EQ(wide_at(7), u"\\\\?\\C:\\");
    const auto none = w(r, 4);   // root flags=4: "\" (ret 1)
    EXPECT_EQ(none.ret, 1u);
    EXPECT_EQ(wide_at(1), u"\\");
}

TEST_F(FinalPathRig, ShortBufferReturnsSizeWithNulAndWritesNothing) {
    const Handle f = open("C:\\Sub Dir\\MixedCase.txt");
    ASSERT_NE(f, INVALID_HANDLE);
    const u32 len = static_cast<u32>(kDosFile.size());
    // sizes_dos: cch 0, 1, len-1, len -> len+1, ERROR_NOT_ENOUGH_MEMORY.
    for (u32 cch : {0u, 1u, len - 1, len}) {
        const auto r = w(f, 0, cch);
        EXPECT_EQ(r.ret, len + 1) << cch;
        EXPECT_EQ(r.err, error_codes::ERROR_NOT_ENOUGH_MEMORY) << cch;
        EXPECT_EQ(touched(), 0u) << cch;
    }
    // cch len+1 and len+2 succeed and write exactly len+1 characters.
    for (u32 cch : {len + 1, len + 2}) {
        const auto r = w(f, 0, cch);
        EXPECT_EQ(r.ret, len) << cch;
        EXPECT_EQ(r.err, 0u) << cch;
        EXPECT_EQ(touched(), (len + 1) * 2) << cch;
    }
    // null_buf_cch0.
    const auto q = call(false, f, 0, 0, 0);
    EXPECT_EQ(q.ret, len + 1);
    EXPECT_EQ(q.err, error_codes::ERROR_NOT_ENOUGH_MEMORY);
    // sizes_none: the same sizes, and the error still becomes 8.
    const u32 nlen = static_cast<u32>(kNoneFile.size());
    const auto s = w(f, 4, nlen);
    EXPECT_EQ(s.ret, nlen + 1);
    EXPECT_EQ(s.err, error_codes::ERROR_NOT_ENOUGH_MEMORY);
    EXPECT_EQ(touched(), 0u);
}

TEST_F(FinalPathRig, VolumeFieldIsValidatedBeforeTheHandle) {
    const Handle f = open("C:\\Sub Dir\\MixedCase.txt");
    ASSERT_NE(f, INVALID_HANDLE);
    for (u32 flags : {3u, 5u, 6u, 7u, 0xFFFFFFFFu}) {
        const auto r = w(f, flags);
        EXPECT_EQ(r.ret, 0u) << flags;
        EXPECT_EQ(r.err, error_codes::ERROR_INVALID_PARAMETER) << flags;
        EXPECT_EQ(touched(), 0u) << flags;
    }
    // bogus_badflags / event_badflags: 87 wins over 6.
    EXPECT_EQ(w(0x12340, 3).err, error_codes::ERROR_INVALID_PARAMETER);
    const Handle ev = handles.allocate(HandleType::Event, nullptr, 0, 0);
    EXPECT_EQ(w(ev, 3).err, error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(FinalPathRig, NonFileHandlesFailWithTheirObjectSpecificError) {
    // null_handle / invalid_handle_value / bogus_handle / event -> 6.
    for (u64 h : {0x0ull, 0xFFFFFFFFull, 0x12340ull}) {
        const auto r = w(h, 0);
        EXPECT_EQ(r.ret, 0u) << h;
        EXPECT_EQ(r.err, error_codes::ERROR_INVALID_HANDLE) << h;
        EXPECT_EQ(touched(), 0u) << h;
    }
    const Handle ev = handles.allocate(HandleType::Event, nullptr, 0, 0);
    EXPECT_EQ(w(ev, 0).err, error_codes::ERROR_INVALID_HANDLE);

    // anon_pipe -> ERROR_BAD_PATHNAME, in every volume form.
    Handle rp = INVALID_HANDLE, wp = INVALID_HANDLE;
    ASSERT_TRUE(files->create_anonymous_pipe(rp, wp, false));
    for (u32 flags : {0u, 4u, 2u}) {
        EXPECT_EQ(w(rp, flags).err, error_codes::ERROR_BAD_PATHNAME) << flags;
    }

    // closed_handle_after -> 6.
    const Handle f = open("C:\\Sub Dir\\MixedCase.txt");
    ASSERT_NE(f, INVALID_HANDLE);
    ASSERT_TRUE(files->close_file(f));
    EXPECT_EQ(w(f, 0).err, error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(FinalPathRig, RenameWhileOpenReportsTheNewName) {
    const Handle f = open("C:\\Sub Dir\\before.txt");
    ASSERT_NE(f, INVALID_HANDLE);
    ASSERT_TRUE(vfs.rename("C:\\Sub Dir\\before.txt", "C:\\Sub Dir\\renamed.txt").is_ok());
    const std::u16string dos = u"\\\\?\\C:\\Sub Dir\\renamed.txt";
    EXPECT_EQ(w(f, 0).ret, dos.size());  // after_rename flags=0 / 8
    EXPECT_EQ(wide_at(dos.size()), dos);
    EXPECT_EQ(w(f, 8).ret, dos.size());
}

TEST_F(FinalPathRig, DuplicatedHandleNamesTheSameFile) {
    const Handle f = open("C:\\Sub Dir\\MixedCase.txt");
    ASSERT_NE(f, INVALID_HANDLE);
    const Handle dup = files->duplicate_handle(f, /*DUPLICATE_SAME_ACCESS*/ 2, 0);
    ASSERT_NE(dup, INVALID_HANDLE);
    EXPECT_EQ(w(dup, 0).ret, kDosFile.size());
    EXPECT_EQ(wide_at(kDosFile.size()), kDosFile);
}

TEST_F(FinalPathRig, UnicodeNamesAreUtf16InWAndAnsiInA) {
    const Handle latin = open("C:\\Sub Dir\\caf\xC3\xA9.txt");
    const Handle cjk = open("C:\\Sub Dir\\\xE4\xB8\xAD.txt");
    ASSERT_NE(latin, INVALID_HANDLE);
    ASSERT_NE(cjk, INVALID_HANDLE);
    const std::u16string wl = u"\\\\?\\C:\\Sub Dir\\caf\u00e9.txt";
    const std::u16string wc = u"\\\\?\\C:\\Sub Dir\\\u4e2d.txt";
    EXPECT_EQ(w(latin, 0).ret, wl.size());
    EXPECT_EQ(wide_at(wl.size()), wl);
    EXPECT_EQ(w(cjk, 0).ret, wc.size());
    EXPECT_EQ(wide_at(wc.size()), wc);
    // A_latin1: caf\xe9; A_cjk: '?' for the unmappable character.
    const auto al = a(latin, 0);
    EXPECT_EQ(al.ret, wl.size());
    EXPECT_EQ(ansi_at(al.ret), std::string("\\\\?\\C:\\Sub Dir\\caf\xE9.txt"));
    const auto ac = a(cjk, 0);
    EXPECT_EQ(ac.ret, wc.size());
    EXPECT_EQ(ansi_at(ac.ret), std::string("\\\\?\\C:\\Sub Dir\\?.txt"));
}

TEST_F(FinalPathRig, AnsiShortBufferReturnsLengthWithoutNulAndSetsNoError) {
    const Handle f = open("C:\\Sub Dir\\MixedCase.txt");
    ASSERT_NE(f, INVALID_HANDLE);
    const u32 len = static_cast<u32>(kDosFile.size());
    // A_file: success writes len+1 bytes and returns len.
    const auto ok = a(f, 0);
    EXPECT_EQ(ok.ret, len);
    EXPECT_EQ(ok.err, 0u);
    EXPECT_EQ(touched(), len + 1);
    // A_file_null_cch0 / A_file_exact / A_file_short: ret len, nothing written,
    // error only what the DOS form set (0).
    const auto q = call(true, f, 0, 0, 0);
    EXPECT_EQ(q.ret, len);
    for (u32 cch : {len, len - 1}) {
        const auto r = a(f, 0, cch);
        EXPECT_EQ(r.ret, len) << cch;
        EXPECT_EQ(r.err, 0u) << cch;
        EXPECT_EQ(touched(), 0u) << cch;
    }
    // A_file_none: success, last error untouched.
    const auto none = a(f, 4);
    EXPECT_EQ(none.ret, kNoneFile.size());
    EXPECT_EQ(none.err, kSentinel);
    // A_badflags / A_event.
    EXPECT_EQ(a(f, 3).err, error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(a(handles.allocate(HandleType::Event, nullptr, 0, 0), 0).err,
              error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(FinalPathRig, SixtyFourBitCallersPassPointerWideHandles) {
    const Handle f = open("C:\\Sub Dir\\MixedCase.txt");
    ASSERT_NE(f, INVALID_HANDLE);
    const auto r = w(f, 0, 1024, /*x64=*/true);
    EXPECT_EQ(r.ret, kDosFile.size());
    EXPECT_EQ(wide_at(kDosFile.size()), kDosFile);
    // invalid_handle_value is all ones in a 64-bit HANDLE.
    const auto bad = w(~0ull, 0, 1024, /*x64=*/true);
    EXPECT_EQ(bad.ret, 0u);
    EXPECT_EQ(bad.err, error_codes::ERROR_INVALID_HANDLE);
}

// file / dir / root flags=2 and 0xA: the drive's NT device -- the one
// QueryDosDevice reports -- then the path, and the last error left alone.
TEST_F(FinalPathRig, NtFormIsTheDrivesDeviceThenThePath) {
    const Handle f = open("C:\\Sub Dir\\MixedCase.txt");
    ASSERT_NE(f, INVALID_HANDLE);
    const std::u16string file = u"\\Device\\HarddiskVolume3\\Sub Dir\\MixedCase.txt";
    for (u32 flags : {2u, 0xAu}) {
        const auto r = w(f, flags);
        EXPECT_EQ(r.ret, file.size()) << flags;
        EXPECT_EQ(r.err, kSentinel) << flags;
        EXPECT_EQ(wide_at(file.size() + 1), file + u'\0') << flags;
    }
    const Handle root_dir = open("C:\\", FILE_FLAG_BACKUP_SEMANTICS_);
    ASSERT_NE(root_dir, INVALID_HANDLE);
    const std::u16string root_nt = u"\\Device\\HarddiskVolume3\\";
    const auto r = w(root_dir, 2);
    EXPECT_EQ(r.ret, root_nt.size());
    EXPECT_EQ(wide_at(root_nt.size()), root_nt);
}

TEST_F(FinalPathRig, GuidFormFailsUntilVolumesHaveGuids) {
    // Recorded gap: Windows answers file flags=1 with \\?\Volume{GUID}\...;
    // Vitreo's volumes have no GUIDs, so it fails with Learn's
    // no-such-volume-name error instead of a made-up one.
    const Handle f = open("C:\\Sub Dir\\MixedCase.txt");
    ASSERT_NE(f, INVALID_HANDLE);
    for (u32 flags : {1u, 9u}) {
        const auto r = w(f, flags);
        EXPECT_EQ(r.ret, 0u) << flags;
        EXPECT_EQ(r.err, error_codes::ERROR_PATH_NOT_FOUND) << flags;
        EXPECT_EQ(touched(), 0u) << flags;
    }
}

namespace {

struct OpticalFileSystem : capsule::HostFileSystem {
    using capsule::HostFileSystem::HostFileSystem;
    [[nodiscard]] bool is_optical() const noexcept override { return true; }
};

}  // namespace

// tools/oracle_probe/query_dos_device_probe.expected (x86 == x64).
class QueryDosDeviceRig : public FinalPathRig {
protected:
    /// QueryDosDeviceW/A(name, kBuf, cch); name nullptr is a NULL name.
    Result2 query(bool ansi, const char* name, u32 cch) {
        constexpr GuestAddr kName = 0x30000;
        if (name != nullptr) {
            for (usize i = 0;; ++i) {
                const u8 c = static_cast<u8>(name[i]);
                EXPECT_TRUE((ansi ? memory.write_u8(kName + i, c)
                                  : memory.write_u16(kName + i * 2, c)).is_ok());
                if (c == 0) break;
            }
        }
        fill(4096);
        cpu::CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.rsp = kStack;
        cpu.gpr.rcx = name != nullptr ? kName : 0;
        cpu.gpr.rdx = kBuf;
        cpu.gpr.r8 = cch;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        set_last_error(kSentinel);
        if (ansi) {
            mgr->impl_QueryDosDeviceA(m);
        } else {
            mgr->impl_QueryDosDeviceW(m);
        }
        return {static_cast<u32>(cpu.gpr.rax), get_last_error()};
    }
};

TEST_F(QueryDosDeviceRig, DriveLettersNameTheirNtDevice) {
    const std::u16string c_device = u"\\Device\\HarddiskVolume3";
    for (const char* name : {"C:", "c:"}) {
        const auto r = query(false, name, 4096);
        EXPECT_EQ(r.ret, 25u) << name;
        EXPECT_EQ(r.err, kSentinel) << name;
        EXPECT_EQ(wide_at(25), c_device + u'\0' + u'\0') << name;
    }
    const auto nul = query(false, "NUL", 4096);
    EXPECT_EQ(nul.ret, 14u);
    EXPECT_EQ(wide_at(14), std::u16string(u"\\Device\\Null") + u'\0' + u'\0');

    const auto a = query(true, "C:", 4096);
    EXPECT_EQ(a.ret, 25u);
    EXPECT_EQ(ansi_at(25), std::string("\\Device\\HarddiskVolume3") + '\0' + '\0');
}

TEST_F(QueryDosDeviceRig, NamesThatAreNotDevicesFail) {
    const std::pair<const char*, u32> rows[] = {
        {"C:\\", error_codes::ERROR_INVALID_HANDLE},
        {"C:\\Windows", error_codes::ERROR_INVALID_HANDLE},
        {"NUL\\", error_codes::ERROR_INVALID_HANDLE},
        {"", error_codes::ERROR_INVALID_HANDLE},
        {"C", error_codes::ERROR_FILE_NOT_FOUND},
        {"Q:", error_codes::ERROR_FILE_NOT_FOUND},
        {"Q:\\", error_codes::ERROR_PATH_NOT_FOUND},
    };
    for (const auto& [name, error] : rows) {
        const auto r = query(false, name, 4096);
        EXPECT_EQ(r.ret, 0u) << name;
        EXPECT_EQ(r.err, error) << name;
        EXPECT_EQ(touched(), 0u) << name;
    }
}

TEST_F(QueryDosDeviceRig, ShortBuffersFailWithInsufficientBuffer) {
    EXPECT_EQ(query(false, "C:", 25).ret, 25u);  // exact fit
    // One short: the string is stored, then the call fails.
    const auto short_one = query(false, "C:", 24);
    EXPECT_EQ(short_one.ret, 0u);
    EXPECT_EQ(short_one.err, error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u16(kBuf).value(), u'\\');
    for (u32 cch : {23u, 0u}) {
        const auto r = query(false, "C:", cch);
        EXPECT_EQ(r.ret, 0u) << cch;
        EXPECT_EQ(r.err, error_codes::ERROR_INSUFFICIENT_BUFFER) << cch;
        EXPECT_EQ(touched(), 0u) << cch;
    }
}

TEST_F(QueryDosDeviceRig, NullNameListsEveryDeviceName) {
    const auto all = query(false, nullptr, 4096);
    ASSERT_GT(all.ret, 0u);
    EXPECT_EQ(all.err, kSentinel);
    const std::u16string list = wide_at(all.ret);
    EXPECT_NE(list.find(std::u16string(u"C:") + u'\0'), std::u16string::npos);
    EXPECT_EQ(list.substr(list.size() - 2), std::u16string(2, u'\0'));
    const auto tiny = query(false, nullptr, 4);
    EXPECT_EQ(tiny.ret, 0u);
    EXPECT_EQ(tiny.err, error_codes::ERROR_INSUFFICIENT_BUFFER);
}

// The oracle's layout (C: HarddiskVolume3, CD-ROM drives CdRom0, CdRom1 in
// letter order); further fixed drives follow C: in letter order.
TEST_F(QueryDosDeviceRig, OpticalDrivesAreCdRomsAndOtherFixedDrivesFollowC) {
    ASSERT_TRUE(vfs.mount_drive('E', std::make_unique<OpticalFileSystem>(root)).is_ok());
    ASSERT_TRUE(vfs.mount_drive('D', std::make_unique<OpticalFileSystem>(root)).is_ok());
    ASSERT_TRUE(vfs.mount_drive('F', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    const std::pair<const char*, std::u16string> rows[] = {
        {"C:", u"\\Device\\HarddiskVolume3"},
        {"D:", u"\\Device\\CdRom0"},
        {"E:", u"\\Device\\CdRom1"},
        {"F:", u"\\Device\\HarddiskVolume4"},
    };
    for (const auto& [name, device] : rows) {
        const auto r = query(false, name, 4096);
        EXPECT_EQ(r.ret, device.size() + 2) << name;
        EXPECT_EQ(wide_at(device.size()), device) << name;
    }
}
