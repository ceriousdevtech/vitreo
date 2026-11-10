#include "user32_ext_thunk_harness.hpp"
#include "winapi/user32/input/cursor_clip_sdk.generated.hpp"

class CursorClipContractTest : public User32ExtThunkTest {
protected:
    struct Result { u32 value, error; };
    Result invoke(const char* name, GuestAddr pointer, bool wide) {
        cpu_.mode = wide ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        if (wide) cpu_.gpr.rcx = pointer;
        else (void)memory_.write_u32(cpu_.gpr.rsp + sizeof(u32), static_cast<u32>(pointer));
        Marshaler m(memory_); m.attach_cpu(cpu_);
        const auto entry = find_export(name);
        EXPECT_EQ(entry.stdcall_bytes, sizeof(u32));
        set_last_error(0xDEAD);
        entry.handler(m);
        return {static_cast<u32>(cpu_.gpr.rax), get_last_error()};
    }
    void write_rect(GuestAddr at, windowing::Rect rect) {
        const std::array<i32, 4> fields{rect.left, rect.top, rect.right, rect.bottom};
        for (usize i = 0; i < fields.size(); ++i)
            ASSERT_TRUE(memory_.write_u32(at + cursor_clip_sdk::kRectOffsets[i],
                static_cast<u32>(fields[i])).is_ok());
    }
    void expect_rect(windowing::Rect expected, bool wide) {
        const auto result = invoke("GetClipCursor", BUF_ADDR + 0x100, wide);
        ASSERT_EQ(result.value, 1u); EXPECT_EQ(result.error, 0xDEADu);
        const std::array<i32, 4> fields{expected.left, expected.top, expected.right, expected.bottom};
        for (usize i = 0; i < fields.size(); ++i)
            EXPECT_EQ(static_cast<i32>(memory_.read_u32(BUF_ADDR + 0x100 +
                cursor_clip_sdk::kRectOffsets[i]).value_or(0)), fields[i]);
    }
    void screen() { mgr_->apply_virtual_display_mode(NULL_HWND, 1920, 1080, 32); }
};

// Expected RECTs, results and errors are from identical Windows x86/x64 run9
// tests/oracle_data/cursor_clip_contract_{x86,x64}.expected.
TEST_F(CursorClipContractTest, RectangleCasesAndFailureStateBothBitnesses) {
    screen();
    struct Case { windowing::Rect requested, effective; };
    const windowing::Rect full{0, 0, 1920, 1080};
    const std::array cases{
        Case{{960,540,961,541},{960,540,961,541}},
        Case{{960,540,960,540},{960,540,960,540}},
        Case{{960,540,960,548},{960,540,960,548}},
        Case{{960,540,968,540},{960,540,968,540}},
        Case{{962,540,960,548},full}, Case{{960,542,968,540},full},
        Case{{962,542,960,540},full}, Case{{-10,-10,10,10},{0,0,10,10}},
        Case{{1940,20,1960,40},full},
        Case{{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX},full},
        Case{{-1,-1,1,1},{0,0,1,1}}, Case{{0,0,0,0},{0,0,0,0}},
        Case{{1940,20,1940,20},full}, Case{full,full},
        Case{{1920,1080,1920,1080},{1920,1080,1920,1080}},
        Case{{-8,540,0,548},{0,540,0,548}}, Case{{-8,0,0,0},{0,0,0,0}}
    };
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        for (usize i = 0; i < cases.size(); ++i) {
            SCOPED_TRACE(i);
            ASSERT_EQ(invoke("ClipCursor", 0, wide).value, 1u);
            write_rect(BUF_ADDR, cases[i].requested);
            const auto result = invoke("ClipCursor", BUF_ADDR, wide);
            EXPECT_EQ(result.value, 1u); EXPECT_EQ(result.error, 0xDEADu);
            expect_rect(cases[i].effective, wide);
        }
        ASSERT_EQ(invoke("ClipCursor", 0, wide).value, 1u);
        write_rect(BUF_ADDR, {960,540,961,541});
        ASSERT_EQ(invoke("ClipCursor", BUF_ADDR, wide).value, 1u);
        const auto bad = invoke("ClipCursor", 1, wide);
        EXPECT_EQ(bad.value, 0u); EXPECT_EQ(bad.error, error_codes::ERROR_NOACCESS);
        expect_rect({960,540,961,541}, wide);
        for (GuestAddr pointer : {GuestAddr{0}, GuestAddr{1}}) {
            const auto get = invoke("GetClipCursor", pointer, wide);
            EXPECT_EQ(get.value, 0u); EXPECT_EQ(get.error, error_codes::ERROR_NOACCESS);
        }
    }
}

TEST_F(CursorClipContractTest, DegenerateRestrictionPrecedesPointerProbe) {
    screen();
    for (bool wide : {false, true}) {
        for (windowing::Rect previous : {windowing::Rect{960,540,960,540}, windowing::Rect{960,540,960,548},
                              windowing::Rect{960,540,968,540}}) {
            for (int incoming = 0; incoming < 5; ++incoming) {
                ASSERT_EQ(invoke("ClipCursor", 0, wide).value, 1u);
                write_rect(BUF_ADDR, previous);
                ASSERT_EQ(invoke("ClipCursor", BUF_ADDR, wide).value, 1u);
                if (incoming != 0) write_rect(BUF_ADDR,
                    incoming == 3 ? windowing::Rect{962,540,960,548} :
                    incoming == 4 ? windowing::Rect{1940,20,1960,40} : windowing::Rect{960,540,961,541});
                const auto result = invoke("ClipCursor", incoming == 0 ? 1 : BUF_ADDR, wide);
                EXPECT_EQ(result.value, 0u); EXPECT_EQ(result.error, error_codes::ERROR_ACCESS_DENIED);
                expect_rect(previous, wide);
                const auto release = invoke("ClipCursor", 0, wide);
                EXPECT_EQ(release.value, 1u); EXPECT_EQ(release.error, 0xDEADu);
            }
        }
    }
}

TEST_F(CursorClipContractTest, ReadonlyAndPartialBuffersDoNotPartiallyWriteOrChangeClip) {
    screen();
    constexpr GuestAddr edge_base = 0x30000;
    constexpr usize page = 0x1000;
    ASSERT_TRUE(memory_.map_region(edge_base, page, MemoryProtection::ReadWrite).is_ok());
    for (bool wide : {false, true}) {
        ASSERT_EQ(invoke("ClipCursor", 0, wide).value, 1u);
        write_rect(BUF_ADDR, {960,540,961,541});
        ASSERT_TRUE(memory_.protect_region(BUF_ADDR, page, MemoryProtection::Read).is_ok());
        const auto read = invoke("ClipCursor", BUF_ADDR, wide);
        EXPECT_EQ(read.value, 1u); EXPECT_EQ(read.error, 0xDEADu);
        ASSERT_TRUE(memory_.protect_region(BUF_ADDR, page, MemoryProtection::ReadWrite).is_ok());
        std::array<Byte, cursor_clip_sdk::kRectSize> sentinel{}; sentinel.fill(0x5A);
        constexpr GuestAddr readonly_output = 0x4000;
        ASSERT_TRUE(memory_.write_bytes(readonly_output, sentinel.data(), sentinel.size()).is_ok());
        ASSERT_TRUE(memory_.protect_region(readonly_output, page, MemoryProtection::Read).is_ok());
        const auto write = invoke("GetClipCursor", readonly_output, wide);
        EXPECT_EQ(write.value, 0u); EXPECT_EQ(write.error, error_codes::ERROR_NOACCESS);
        std::array<Byte, cursor_clip_sdk::kRectSize> actual{};
        ASSERT_TRUE(memory_.read_bytes(readonly_output, actual.data(), actual.size()).is_ok());
        EXPECT_EQ(actual, sentinel);
        ASSERT_TRUE(memory_.protect_region(readonly_output, page, MemoryProtection::ReadWrite).is_ok());
        for (usize available : {usize{0}, usize{4}, usize{8}, usize{12}, usize{16}}) {
            SCOPED_TRACE(available);
            ASSERT_EQ(invoke("ClipCursor", 0, wide).value, 1u);
            write_rect(BUF_ADDR, {960,540,961,541});
            ASSERT_EQ(invoke("ClipCursor", BUF_ADDR, wide).value, 1u);
            const GuestAddr at = edge_base + page - available;
            if (available) ASSERT_TRUE(memory_.write_bytes(at, sentinel.data(), available).is_ok());
            const auto get = invoke("GetClipCursor", at, wide);
            EXPECT_EQ(get.value, available == cursor_clip_sdk::kRectSize ? 1u : 0u);
            EXPECT_EQ(get.error, available == cursor_clip_sdk::kRectSize ? 0xDEADu : error_codes::ERROR_NOACCESS);
            if (available < cursor_clip_sdk::kRectSize && available) {
                actual.fill(0);
                ASSERT_TRUE(memory_.read_bytes(at, actual.data(), available).is_ok());
                for (usize i = 0; i < available; ++i) EXPECT_EQ(actual[i], 0x5A);
            }
            if (available == cursor_clip_sdk::kRectSize) write_rect(at, {960,540,960,540});
            const auto clip = invoke("ClipCursor", at, wide);
            EXPECT_EQ(clip.value, available == cursor_clip_sdk::kRectSize ? 1u : 0u);
            EXPECT_EQ(clip.error, available == cursor_clip_sdk::kRectSize ? 0xDEADu : error_codes::ERROR_NOACCESS);
            expect_rect(available == cursor_clip_sdk::kRectSize ? windowing::Rect{960,540,960,540} : windowing::Rect{960,540,961,541}, wide);
        }
    }
}
