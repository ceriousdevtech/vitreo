/// @file test_pathcch_thunks.cpp
/// @brief Guest-boundary tests for the `PathCch*` exports.
///
/// The path algebra itself is covered by test_pathcch.cpp. What is proven here
/// is the part a guest actually observes: a real HRESULT in EAX/RAX instead of
/// the missing-export stub's success-shaped 0xFFFFFFFF, UTF-16 written back
/// into the caller's own buffer, and `PCWSTR*` out-parameters carrying a
/// pointer *into that guest buffer* at the guest's pointer width.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/path/pathcch.hpp"
#include "winapi/kernel32/path/pathcch_core.hpp"

#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kArena = 0x0001'0000;
constexpr GuestAddr kStack = 0x0002'0000;

class PathCchThunkTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kArena, 0x1000, MemoryProtection::ReadWrite)
                        .is_ok());
        ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite)
                        .is_ok());
        cpu.init_long_mode();
        cpu.gpr.rsp = kStack + 0x800;
        marshaler.attach_cpu(cpu);
        exports = make_pathcch_exports();
    }

    [[nodiscard]] const ApiHandler& handler(const std::string& name) const {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) return entry.handler;
        }
        ADD_FAILURE() << "export not registered: " << name;
        static const ApiHandler none = [](Marshaler&) {};
        return none;
    }

    /// Place a UTF-16 string at `addr` and return the address back.
    GuestAddr put(GuestAddr addr, const std::u16string& value) {
        for (usize i = 0; i < value.size(); ++i) {
            EXPECT_TRUE(
                memory.write_u16(addr + static_cast<GuestAddr>(i * 2), value[i])
                    .is_ok());
        }
        EXPECT_TRUE(
            memory.write_u16(addr + static_cast<GuestAddr>(value.size() * 2), 0)
                .is_ok());
        return addr;
    }

    [[nodiscard]] std::string get(GuestAddr addr) {
        std::string out;
        for (usize i = 0; i < 512; ++i) {
            auto unit = memory.read_u16(addr + static_cast<GuestAddr>(i * 2));
            EXPECT_TRUE(unit.is_ok());
            if (!unit.is_ok() || unit.value() == 0) break;
            out.push_back(static_cast<char>(unit.value() & 0x7F));
        }
        return out;
    }

    /// Fill the arena with a non-zero pattern so a "left untouched" buffer is
    /// distinguishable from a written one.
    void poison(GuestAddr addr, usize cch) {
        for (usize i = 0; i < cch; ++i) {
            EXPECT_TRUE(
                memory.write_u16(addr + static_cast<GuestAddr>(i * 2), u'Z')
                    .is_ok());
        }
    }

    void set_args(u64 a0, u64 a1 = 0, u64 a2 = 0, u64 a3 = 0) {
        cpu.gpr.rcx = a0;
        cpu.gpr.rdx = a1;
        cpu.gpr.r8 = a2;
        cpu.gpr.r9 = a3;
    }

    /// x64 stack argument 5 (index 4) lives at RSP + 0x28.
    void set_arg4(u64 value) {
        ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, value).is_ok());
    }

    [[nodiscard]] u32 result() const { return static_cast<u32>(cpu.gpr.rax); }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports;
};

} // namespace

TEST_F(PathCchThunkTest, EveryDocumentedExportIsRegistered) {
    const char* names[] = {
        "PathCchCombineEx",       "PathCchCombine",
        "PathCchSkipRoot",        "PathCchAppend",
        "PathCchAppendEx",        "PathCchAddBackslash",
        "PathCchAddBackslashEx",  "PathCchRemoveBackslash",
        "PathCchRemoveBackslashEx", "PathCchCanonicalize",
        "PathCchCanonicalizeEx",  "PathCchFindExtension",
        "PathCchRenameExtension", "PathCchAddExtension",
        "PathCchRemoveExtension", "PathCchRemoveFileSpec",
        "PathCchStripPrefix",     "PathCchStripToRoot",
        "PathCchIsRoot",          "PathIsUNCEx",
    };
    for (const char* name : names) {
        bool found = false;
        for (const auto& entry : exports.entries) {
            if (entry.name == name) found = true;
        }
        EXPECT_TRUE(found) << name;
    }
}

TEST_F(PathCchThunkTest, CombineExWritesCombinedPath) {
    const GuestAddr out = kArena;
    const GuestAddr in = kArena + 0x200;
    const GuestAddr more = kArena + 0x300;
    put(in, u"C:\\dir\\sub");
    put(more, u"..\\file.txt");

    set_args(out, 64, in, more);
    set_arg4(pathcch::kNone);
    handler("PathCchCombineEx")(marshaler);

    EXPECT_EQ(result(), pathcch::kSOk);
    EXPECT_EQ(get(out), "C:\\dir\\file.txt");
}

TEST_F(PathCchThunkTest, CombineExReportsInsufficientBufferNotSuccess) {
    // This is the reported failure mode: the missing-export stub returned
    // 0xFFFFFFFF, which the caller read as success. A real too-small buffer
    // must return STRSAFE_E_INSUFFICIENT_BUFFER and leave nothing usable.
    const GuestAddr out = kArena;
    const GuestAddr in = kArena + 0x200;
    const GuestAddr more = kArena + 0x300;
    poison(out, 32);
    put(in, u"C:\\dir");
    put(more, u"file.txt");

    set_args(out, 8, in, more); // "C:\dir\file.txt" needs 15 characters
    set_arg4(pathcch::kNone);
    handler("PathCchCombineEx")(marshaler);

    EXPECT_EQ(result(), pathcch::kInsufficientBuffer);
    EXPECT_NE(result(), 0xFFFFFFFFu);
    EXPECT_EQ(get(out), "");
}

TEST_F(PathCchThunkTest, CombineExRejectsBadCchAndNullOutput) {
    const GuestAddr out = kArena;
    const GuestAddr in = kArena + 0x200;
    put(in, u"C:\\dir");

    set_args(out, 0, in, 0);
    set_arg4(pathcch::kNone);
    handler("PathCchCombineEx")(marshaler);
    EXPECT_EQ(result(), pathcch::kEInvalidArg);

    set_args(out, pathcch::kPathCchMaxCch + 1, in, 0);
    set_arg4(pathcch::kNone);
    handler("PathCchCombineEx")(marshaler);
    EXPECT_EQ(result(), pathcch::kEInvalidArg);

    set_args(0, 64, in, 0);
    set_arg4(pathcch::kNone);
    handler("PathCchCombineEx")(marshaler);
    EXPECT_EQ(result(), pathcch::kEInvalidArg);
}

TEST_F(PathCchThunkTest, CombineExWithBothPathsNullYieldsBackslash) {
    const GuestAddr out = kArena;
    set_args(out, 64, 0, 0);
    set_arg4(pathcch::kNone);
    handler("PathCchCombineEx")(marshaler);

    EXPECT_EQ(result(), pathcch::kSOk);
    EXPECT_EQ(get(out), "\\");
}

TEST_F(PathCchThunkTest, SkipRootWritesGuestPointerIntoInputBuffer) {
    const GuestAddr path = kArena;
    const GuestAddr out = kArena + 0x400;
    put(path, u"\\\\server\\share\\dir");

    set_args(path, out);
    handler("PathCchSkipRoot")(marshaler);

    EXPECT_EQ(result(), pathcch::kSOk);
    const auto stored = memory.read_u64(out);
    ASSERT_TRUE(stored.is_ok());
    // The root is 15 characters, so the remainder starts 30 bytes in.
    EXPECT_EQ(stored.value(), static_cast<u64>(path) + 30);
    EXPECT_EQ(get(static_cast<GuestAddr>(stored.value())), "dir");
}

TEST_F(PathCchThunkTest, SkipRootRejectsRootlessPath) {
    const GuestAddr path = kArena;
    const GuestAddr out = kArena + 0x400;
    put(path, u"relative\\path");

    set_args(path, out);
    handler("PathCchSkipRoot")(marshaler);
    EXPECT_EQ(result(), pathcch::kEInvalidArg);
}

TEST_F(PathCchThunkTest, AddBackslashExReportsEndPointerAndRemaining) {
    const GuestAddr path = kArena;
    const GuestAddr end_out = kArena + 0x400;
    const GuestAddr remaining_out = kArena + 0x420;
    put(path, u"C:\\dir");

    set_args(path, 10, end_out, remaining_out);
    handler("PathCchAddBackslashEx")(marshaler);

    EXPECT_EQ(result(), pathcch::kSOk);
    EXPECT_EQ(get(path), "C:\\dir\\");
    const auto end = memory.read_u64(end_out);
    const auto remaining = memory.read_u64(remaining_out);
    ASSERT_TRUE(end.is_ok());
    ASSERT_TRUE(remaining.is_ok());
    EXPECT_EQ(end.value(), static_cast<u64>(path) + 14); // 7 characters in
    EXPECT_EQ(remaining.value(), 3u);
}

TEST_F(PathCchThunkTest, InPlaceExportsMutateTheGuestBuffer) {
    const GuestAddr path = kArena;

    put(path, u"C:\\dir\\file.txt");
    set_args(path, 64);
    handler("PathCchRemoveFileSpec")(marshaler);
    EXPECT_EQ(result(), pathcch::kSOk);
    EXPECT_EQ(get(path), "C:\\dir");

    put(path, u"\\\\?\\C:\\dir");
    set_args(path, 64);
    handler("PathCchStripPrefix")(marshaler);
    EXPECT_EQ(result(), pathcch::kSOk);
    EXPECT_EQ(get(path), "C:\\dir");

    put(path, u"C:\\dir\\sub\\file");
    set_args(path, 64);
    handler("PathCchStripToRoot")(marshaler);
    EXPECT_EQ(result(), pathcch::kSOk);
    EXPECT_EQ(get(path), "C:\\");
}

TEST_F(PathCchThunkTest, BooleanExportsReturnBool) {
    const GuestAddr path = kArena;
    const GuestAddr server_out = kArena + 0x400;

    put(path, u"C:\\");
    set_args(path);
    handler("PathCchIsRoot")(marshaler);
    EXPECT_EQ(result(), 1u);

    put(path, u"C:\\dir");
    set_args(path);
    handler("PathCchIsRoot")(marshaler);
    EXPECT_EQ(result(), 0u);

    set_args(0);
    handler("PathCchIsRoot")(marshaler);
    EXPECT_EQ(result(), 0u);

    put(path, u"\\\\server\\share");
    set_args(path, server_out);
    handler("PathIsUNCEx")(marshaler);
    EXPECT_EQ(result(), 1u);
    const auto server = memory.read_u64(server_out);
    ASSERT_TRUE(server.is_ok());
    EXPECT_EQ(get(static_cast<GuestAddr>(server.value())), "server\\share");

    put(path, u"C:\\dir");
    set_args(path, server_out);
    handler("PathIsUNCEx")(marshaler);
    EXPECT_EQ(result(), 0u);
}

TEST_F(PathCchThunkTest, FindExtensionPointsIntoTheGuestString) {
    const GuestAddr path = kArena;
    const GuestAddr out = kArena + 0x400;

    put(path, u"C:\\dir\\file.txt");
    set_args(path, 64, out);
    handler("PathCchFindExtension")(marshaler);
    EXPECT_EQ(result(), pathcch::kSOk);
    auto ext = memory.read_u64(out);
    ASSERT_TRUE(ext.is_ok());
    EXPECT_EQ(get(static_cast<GuestAddr>(ext.value())), ".txt");

    // No extension: the pointer addresses the terminating null.
    put(path, u"C:\\dir\\file");
    set_args(path, 64, out);
    handler("PathCchFindExtension")(marshaler);
    EXPECT_EQ(result(), pathcch::kSOk);
    ext = memory.read_u64(out);
    ASSERT_TRUE(ext.is_ok());
    EXPECT_EQ(ext.value(), static_cast<u64>(path) + 22);
    EXPECT_EQ(get(static_cast<GuestAddr>(ext.value())), "");
}
