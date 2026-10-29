/// @file test_comctl32.cpp
/// @brief Unit tests for COMCTL32.dll common controls API stubs

#include <gtest/gtest.h>
#include <winapi/user32/windows/sdk_creation_layouts.hpp>
#include <vitreo/common.hpp>
#include <winapi/ui/comctl32/comctl32.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/user32/resources/resources.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <windowing/model/hwnd_manager.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// ICC (InitCommonControlsEx) flag constants from Windows headers
// =============================================================================

static constexpr u32 ICC_LISTVIEW_CLASSES   = 0x00000001; // listview, header
static constexpr u32 ICC_TREEVIEW_CLASSES   = 0x00000002; // treeview, tooltips
static constexpr u32 ICC_BAR_CLASSES        = 0x00000004; // toolbar, statusbar, trackbar, tooltips
static constexpr u32 ICC_TAB_CLASSES        = 0x00000008; // tab, tooltips
static constexpr u32 ICC_UPDOWN_CLASS       = 0x00000010; // updown
static constexpr u32 ICC_PROGRESS_CLASS     = 0x00000020; // progress
static constexpr u32 ICC_HOTKEY_CLASS       = 0x00000040; // hotkey
static constexpr u32 ICC_ANIMATE_CLASS      = 0x00000080; // animate
static constexpr u32 ICC_WIN95_CLASSES      = 0x000000FF; // all Win95 classes
static constexpr u32 ICC_DATE_CLASSES       = 0x00000100; // datetime, monthcal
static constexpr u32 ICC_USEREX_CLASSES     = 0x00000200; // comboex
static constexpr u32 ICC_COOL_CLASSES       = 0x00000400; // rebar
static constexpr u32 ICC_INTERNET_CLASSES   = 0x00000800; // ip address
static constexpr u32 ICC_PAGESCROLLER_CLASS = 0x00001000; // pager
static constexpr u32 ICC_NATIVEFNTCTL_CLASS = 0x00002000; // native font (Win2000+)
static constexpr u32 ICC_STANDARD_CLASSES   = 0x00004000; // button, edit, static, listbox, combobox
static constexpr u32 ICC_LINK_CLASS         = 0x00008000; // syslink (WinXP+)

// INITCOMMONCONTROLSEX structure (8 bytes on x86)
#pragma pack(push, 1)
struct INITCOMMONCONTROLSEX {
    u32 dwSize;     // Size of structure
    u32 dwICC;      // ICC_* flags
};
#pragma pack(pop)

static_assert(sizeof(INITCOMMONCONTROLSEX) == 8, "INITCOMMONCONTROLSEX must be 8 bytes");

// =============================================================================
// Test Fixture
// =============================================================================

class ComCtl32Test : public ::testing::Test {
protected:
    void SetUp() override {
        // Map memory for guest operations
        auto res = memory.map_region(DATA_BASE, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());

        // Map stack region
        auto stack_res = memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());

        // Set up 64-bit mode CPU state with stack (default is long mode)
        cpu.init_long_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;

        // Attach CPU to marshaler
        marshaler.attach_cpu(cpu);
        mgr.set_resource_manager(&resource_mgr);
        mgr.set_hwnd_manager(&hwnd_mgr);
        previous_tid = ThreadManager::get_current_thread_id_static();
        ThreadManager::set_current_thread_id(1);
    }

    void TearDown() override {
        ThreadManager::set_current_thread_id(previous_tid);
    }

    windowing::HWND create_window(u32 owner_thread_id = 1) {
        windowing::WindowData data{};
        data.owner_thread_id = owner_thread_id;
        data.wndproc = BASE_WNDPROC;
        return hwnd_mgr.create(data);
    }

    u64 call_def_subclass_proc(
        windowing::HWND hwnd, u32 msg, u64 w_param, u64 l_param
    ) {
        cpu.gpr.rcx = hwnd;
        cpu.gpr.rdx = msg;
        cpu.gpr.r8 = w_param;
        cpu.gpr.r9 = l_param;
        mgr.impl_DefSubclassProc(marshaler);
        return cpu.gpr.rax;
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE = 0x1000;
    static constexpr GuestAddr BASE_WNDPROC = 0x401000;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    ResourceManager resource_mgr{memory};
    windowing::HwndManager hwnd_mgr;
    ComCtl32Manager mgr;
    u32 previous_tid = 0;
};

// =============================================================================
// InitCommonControls Tests
// =============================================================================

TEST_F(ComCtl32Test, InitCommonControlsSucceeds) {
    // InitCommonControls is a no-op — just verify it doesn't crash
    mgr.impl_InitCommonControls(marshaler);
    // No return value to check — void function
    SUCCEED();
}

// =============================================================================
// InitCommonControlsEx Tests
// =============================================================================

TEST_F(ComCtl32Test, InitCommonControlsExReturnsTrue) {
    // Set up INITCOMMONCONTROLSEX structure in guest memory
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_WIN95_CLASSES;

    constexpr GuestAddr icc_addr = DATA_BASE + 0x100;
    (void)memory.write_bytes(icc_addr, reinterpret_cast<const Byte*>(&icc), sizeof(icc));

    // Pass structure pointer as first argument (RCX in x64)
    cpu.gpr.rcx = icc_addr;

    mgr.impl_InitCommonControlsEx(marshaler);

    // Should return TRUE (1)
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(ComCtl32Test, InitCommonControlsExWithNullPointerReturnsFalse) {
    // NULL lpInitCtrls is invalid per Win32 contract
    cpu.gpr.rcx = 0;

    mgr.impl_InitCommonControlsEx(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(ComCtl32Test, InitCommonControlsExWithStandardClasses) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_STANDARD_CLASSES;

    constexpr GuestAddr icc_addr = DATA_BASE + 0x200;
    (void)memory.write_bytes(icc_addr, reinterpret_cast<const Byte*>(&icc), sizeof(icc));

    cpu.gpr.rcx = icc_addr;
    mgr.impl_InitCommonControlsEx(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(ComCtl32Test, InitCommonControlsExWithAllClasses) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_DATE_CLASSES | ICC_USEREX_CLASSES |
                ICC_COOL_CLASSES | ICC_INTERNET_CLASSES | ICC_PAGESCROLLER_CLASS |
                ICC_NATIVEFNTCTL_CLASS | ICC_STANDARD_CLASSES | ICC_LINK_CLASS;

    constexpr GuestAddr icc_addr = DATA_BASE + 0x300;
    (void)memory.write_bytes(icc_addr, reinterpret_cast<const Byte*>(&icc), sizeof(icc));

    cpu.gpr.rcx = icc_addr;
    mgr.impl_InitCommonControlsEx(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
}

// =============================================================================
// ImageList_Create Tests
// =============================================================================

TEST_F(ComCtl32Test, ImageListCreateReturnsHandle) {
    // ImageList_Create(cx, cy, flags, cInitial, cGrow)
    cpu.gpr.rcx = 16;   // cx = 16 pixels
    cpu.gpr.rdx = 16;   // cy = 16 pixels
    cpu.gpr.r8 = 0x21;  // flags = ILC_COLOR32 | ILC_MASK
    cpu.gpr.r9 = 10;    // cInitial = 10 images
    // 5th arg would be on stack

    mgr.impl_ImageList_Create(marshaler);

    // Should return a non-zero handle
    EXPECT_NE(cpu.gpr.rax, 0u);
}

TEST_F(ComCtl32Test, ImageListCreateReturnsStubHandle) {
    cpu.gpr.rcx = 32;
    cpu.gpr.rdx = 32;

    mgr.impl_ImageList_Create(marshaler);

    // Currently returns fixed stub handle 0xABCD0001
    EXPECT_EQ(cpu.gpr.rax, 0xABCD0001u);
}

TEST_F(ComCtl32Test, ImageListCreateWithZeroDimensions) {
    cpu.gpr.rcx = 0;    // cx = 0
    cpu.gpr.rdx = 0;    // cy = 0

    mgr.impl_ImageList_Create(marshaler);

    // Stub still returns handle (doesn't validate dimensions)
    EXPECT_EQ(cpu.gpr.rax, 0xABCD0001u);
}

TEST_F(ComCtl32Test, ImageListCreateWithLargeDimensions) {
    cpu.gpr.rcx = 256;
    cpu.gpr.rdx = 256;

    mgr.impl_ImageList_Create(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0xABCD0001u);
}

// =============================================================================
// ImageList_Destroy Tests
// =============================================================================

TEST_F(ComCtl32Test, ImageListSetIconSizeUpdatesSizeAndClearsCount) {
    // Learn: ImageList_SetIconSize sets dimensions and removes all images.
    cpu.gpr.rcx = 16;
    cpu.gpr.rdx = 16;
    cpu.gpr.r8 = 0x21;
    cpu.gpr.r9 = 0;
    mgr.impl_ImageList_Create(marshaler);
    u32 himl = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(himl, 0u);

    cpu.gpr.rcx = himl;
    cpu.gpr.rdx = 0; // hbmImage
    cpu.gpr.r8 = 0;  // crMask
    mgr.impl_ImageList_AddMasked(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = himl;
    cpu.gpr.rdx = 32;
    cpu.gpr.r8 = 32;
    mgr.impl_ImageList_SetIconSize(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    GuestAddr cx_addr = DATA_BASE + 0x600;
    GuestAddr cy_addr = DATA_BASE + 0x604;
    (void)memory.write_u32(cx_addr, 0xFFFFFFFFu);
    (void)memory.write_u32(cy_addr, 0xFFFFFFFFu);
    cpu.gpr.rcx = himl;
    cpu.gpr.rdx = cx_addr;
    cpu.gpr.r8 = cy_addr;
    mgr.impl_ImageList_GetIconSize(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(cx_addr).value(), 32u);
    EXPECT_EQ(memory.read_u32(cy_addr).value(), 32u);

    cpu.gpr.rcx = himl;
    mgr.impl_ImageList_GetImageCount(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(ComCtl32Test, ImageListSetIconSizeUnknownHandleReturnsFalse) {
    cpu.gpr.rcx = 0xDEAD;
    cpu.gpr.rdx = 16;
    cpu.gpr.r8 = 16;
    mgr.impl_ImageList_SetIconSize(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(ComCtl32Test, ImageListDestroyReturnsTrue) {
    // First create an image list so we have a valid handle to destroy
    cpu.gpr.rcx = 16;  // cx
    cpu.gpr.rdx = 16;  // cy
    cpu.gpr.r8  = 0x21; // ILC_COLOR32 | ILC_MASK
    cpu.gpr.r9  = 4;   // cInitial
    mgr.impl_ImageList_Create(marshaler);
    u32 himl = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(himl, 0u);

    // Now destroy it
    cpu.gpr.rcx = himl;
    mgr.impl_ImageList_Destroy(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u); // TRUE — valid handle
}

TEST_F(ComCtl32Test, ImageListDestroyWithNullHandle) {
    cpu.gpr.rcx = 0;

    mgr.impl_ImageList_Destroy(marshaler);

    // NULL is not a valid handle — should return FALSE
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(ComCtl32Test, ImageListDestroyWithInvalidHandle) {
    cpu.gpr.rcx = 0xDEADBEEF;

    mgr.impl_ImageList_Destroy(marshaler);

    // Unknown handle — should return FALSE
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// =============================================================================
// ImageList_AddMasked Tests
// =============================================================================

TEST_F(ComCtl32Test, ImageListAddMaskedReturnsIndex) {
    // ImageList_AddMasked(himl, hbmImage, crMask)
    cpu.gpr.rcx = 0xABCD0001;   // himl
    cpu.gpr.rdx = 0x12340001;   // hbmImage (bitmap handle)
    cpu.gpr.r8 = 0x00FF00FF;    // crMask (magenta)

    mgr.impl_ImageList_AddMasked(marshaler);

    // Returns 0 (first image index)
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(ComCtl32Test, ImageListAddMaskedWithNullImageList) {
    cpu.gpr.rcx = 0;            // NULL himl
    cpu.gpr.rdx = 0x12340001;
    cpu.gpr.r8 = 0x00FF00FF;

    mgr.impl_ImageList_AddMasked(marshaler);

    // Current stub returns 0 regardless
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(ComCtl32Test, ImageListAddMaskedWithNullBitmap) {
    cpu.gpr.rcx = 0xABCD0001;
    cpu.gpr.rdx = 0;            // NULL bitmap
    cpu.gpr.r8 = 0;

    mgr.impl_ImageList_AddMasked(marshaler);

    // Stub returns 0 (would return -1 on real failure)
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(ComCtl32Test, ImageListGetImageCountTracksAdds) {
    // Learn: ImageList_GetImageCount returns the number of images.
    cpu.gpr.rcx = 16; cpu.gpr.rdx = 16; cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 1;
    // 5th arg (cGrow) via stack for x64 — check how Create test does it
    mgr.impl_ImageList_Create(marshaler);
    const u32 himl = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(himl, 0u);

    cpu.gpr.rcx = himl;
    mgr.impl_ImageList_GetImageCount(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = himl;
    cpu.gpr.rdx = 0x12340001;
    cpu.gpr.r8 = 0x00FF00FF;
    mgr.impl_ImageList_AddMasked(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u); // index of first image

    cpu.gpr.rcx = himl;
    mgr.impl_ImageList_GetImageCount(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(ComCtl32Test, ImageListDrawSucceedsForTrackedList) {
    cpu.gpr.rcx = 16; cpu.gpr.rdx = 16; cpu.gpr.r8 = 0; cpu.gpr.r9 = 1;
    mgr.impl_ImageList_Create(marshaler);
    const u32 himl = static_cast<u32>(cpu.gpr.rax);

    cpu.gpr.rcx = himl;
    cpu.gpr.rdx = 0;       // index
    cpu.gpr.r8 = 0x100;    // hdc
    cpu.gpr.r9 = 0;        // x
    mgr.impl_ImageList_Draw(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    cpu.gpr.rcx = 0; // invalid
    mgr.impl_ImageList_Draw(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(ComCtl32Test, ThunkHasImageListGetImageCount) {
    auto exports = create_comctl32_thunks(mgr);
    auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "ImageList_GetImageCount"; });
    ASSERT_NE(it, exports.entries.end());
}

TEST_F(ComCtl32Test, ThunkHasImageListDraw) {
    auto exports = create_comctl32_thunks(mgr);
    auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "ImageList_Draw"; });
    ASSERT_NE(it, exports.entries.end());
}

// =============================================================================
// ICC Flag Constants Tests
// =============================================================================

TEST_F(ComCtl32Test, ICCFlagsHaveCorrectValues) {
    // Verify ICC_* constants match Windows SDK values
    EXPECT_EQ(ICC_LISTVIEW_CLASSES,   0x00000001u);
    EXPECT_EQ(ICC_TREEVIEW_CLASSES,   0x00000002u);
    EXPECT_EQ(ICC_BAR_CLASSES,        0x00000004u);
    EXPECT_EQ(ICC_TAB_CLASSES,        0x00000008u);
    EXPECT_EQ(ICC_UPDOWN_CLASS,       0x00000010u);
    EXPECT_EQ(ICC_PROGRESS_CLASS,     0x00000020u);
    EXPECT_EQ(ICC_HOTKEY_CLASS,       0x00000040u);
    EXPECT_EQ(ICC_ANIMATE_CLASS,      0x00000080u);
    EXPECT_EQ(ICC_WIN95_CLASSES,      0x000000FFu);
    EXPECT_EQ(ICC_DATE_CLASSES,       0x00000100u);
    EXPECT_EQ(ICC_USEREX_CLASSES,     0x00000200u);
    EXPECT_EQ(ICC_COOL_CLASSES,       0x00000400u);
    EXPECT_EQ(ICC_INTERNET_CLASSES,   0x00000800u);
    EXPECT_EQ(ICC_PAGESCROLLER_CLASS, 0x00001000u);
    EXPECT_EQ(ICC_NATIVEFNTCTL_CLASS, 0x00002000u);
    EXPECT_EQ(ICC_STANDARD_CLASSES,   0x00004000u);
    EXPECT_EQ(ICC_LINK_CLASS,         0x00008000u);
}

TEST_F(ComCtl32Test, ICCWin95ClassesIsUnionOfBasicClasses) {
    u32 expected = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES |
                   ICC_BAR_CLASSES | ICC_TAB_CLASSES |
                   ICC_UPDOWN_CLASS | ICC_PROGRESS_CLASS |
                   ICC_HOTKEY_CLASS | ICC_ANIMATE_CLASS;
    EXPECT_EQ(ICC_WIN95_CLASSES, expected);
}

// =============================================================================
// Thunk Creation Tests
// =============================================================================

TEST_F(ComCtl32Test, ThunksContainExpectedEntries) {
    DllExports exports = create_comctl32_thunks(mgr);

    // Should have at least 5 exports
    EXPECT_GE(exports.entries.size(), 5u);
}

TEST_F(ComCtl32Test, ThunkHasInitCommonControls) {
    DllExports exports = create_comctl32_thunks(mgr);

    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "InitCommonControls"; });

    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->ordinal, 17u);
    EXPECT_EQ(it->stdcall_bytes, 0u);  // void function
}

TEST_F(ComCtl32Test, ThunkHasInitCommonControlsEx) {
    DllExports exports = create_comctl32_thunks(mgr);

    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "InitCommonControlsEx"; });

    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 4u);  // 1 pointer arg
}

TEST_F(ComCtl32Test, ThunkHasImageListCreate) {
    DllExports exports = create_comctl32_thunks(mgr);

    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "ImageList_Create"; });

    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 20u);  // 5 DWORD args
}

TEST_F(ComCtl32Test, ThunkHasImageListDestroy) {
    DllExports exports = create_comctl32_thunks(mgr);

    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "ImageList_Destroy"; });

    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 4u);  // 1 pointer arg
}

TEST_F(ComCtl32Test, ThunkHasImageListAddMasked) {
    DllExports exports = create_comctl32_thunks(mgr);

    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "ImageList_AddMasked"; });

    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 12u);  // 3 args
}

TEST_F(ComCtl32Test, LoadIconMetricIsExportedByNameAndOrdinal380) {
    DllExports exports = create_comctl32_thunks(mgr);
    const auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "LoadIconMetric";
        });

    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->ordinal, 380u);
    EXPECT_EQ(it->stdcall_bytes, 16u);
}

TEST_F(ComCtl32Test, LoadIconMetricLoadsSystemIconAndWritesX64Handle) {
    constexpr GuestAddr icon_out = DATA_BASE + 0x200;
    ASSERT_TRUE(memory.write_u64(icon_out, 0xCCCCCCCCCCCCCCCCull).is_ok());

    DllExports exports = create_comctl32_thunks(mgr);
    const auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.ordinal == 380;
        });
    ASSERT_NE(it, exports.entries.end());

    cpu.gpr.rcx = 0; // predefined system icon
    cpu.gpr.rdx = idi::IDI_APPLICATION;
    cpu.gpr.r8 = 0; // LIM_SMALL
    cpu.gpr.r9 = icon_out;
    it->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_NE(memory.read_u64(icon_out).value(), 0u);
}

TEST_F(ComCtl32Test, LoadIconMetricRejectsInvalidMetricAndClearsOutput) {
    constexpr GuestAddr icon_out = DATA_BASE + 0x220;
    ASSERT_TRUE(memory.write_u64(icon_out, 0xCCCCCCCCCCCCCCCCull).is_ok());

    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = idi::IDI_APPLICATION;
    cpu.gpr.r8 = 2; // invalid LOADICONMETRIC value
    cpu.gpr.r9 = icon_out;
    mgr.impl_LoadIconMetric(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80070057u);
    // Invalid metric is rejected before the out pointer is touched.
    EXPECT_EQ(memory.read_u64(icon_out).value(), 0xCCCCCCCCCCCCCCCCull);
}

// =============================================================================
// Window subclass helper tests
// =============================================================================

TEST_F(ComCtl32Test, SubclassExportsUseDocumentedOrdinals) {
    const DllExports exports = create_comctl32_thunks(mgr);
    const auto check = [&exports](
        const char* name, u16 ordinal, u32 stdcall_bytes
    ) {
        const auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) {
                return entry.name == name;
            });
        ASSERT_NE(it, exports.entries.end());
        EXPECT_EQ(it->ordinal, ordinal);
        EXPECT_EQ(it->stdcall_bytes, stdcall_bytes);
    };

    check("SetWindowSubclass", 410, 16);
    check("GetWindowSubclass", 411, 16);
    check("RemoveWindowSubclass", 412, 12);
    check("DefSubclassProc", 413, 16);
}

TEST_F(ComCtl32Test, SetWindowSubclassUpdatesExistingReferenceData) {
    const auto hwnd = create_window();
    constexpr GuestAddr proc = 0x402000;
    ASSERT_TRUE(mgr.set_window_subclass(hwnd, proc, 7, 0x1111));
    ASSERT_TRUE(mgr.set_window_subclass(hwnd, proc, 7, 0x2222));

    u64 ref_data = 0;
    EXPECT_TRUE(mgr.get_window_subclass(hwnd, proc, 7, ref_data));
    EXPECT_EQ(ref_data, 0x2222u);
    EXPECT_TRUE(mgr.remove_window_subclass(hwnd, proc, 7));
    EXPECT_FALSE(mgr.remove_window_subclass(hwnd, proc, 7));
}

TEST_F(ComCtl32Test, SetWindowSubclassRejectsCrossThreadWindow) {
    const auto hwnd = create_window(9);
    EXPECT_FALSE(mgr.set_window_subclass(hwnd, 0x402000, 1, 0));
}

TEST_F(ComCtl32Test, DefSubclassProcWalksLifoChainThenOriginalWndProc) {
    const auto hwnd = create_window();
    constexpr GuestAddr older_proc = 0x402000;
    constexpr GuestAddr newer_proc = 0x403000;
    ASSERT_TRUE(mgr.set_window_subclass(hwnd, older_proc, 10, 0xAAAA));
    ASSERT_TRUE(mgr.set_window_subclass(hwnd, newer_proc, 20, 0xBBBB));

    std::vector<GuestAddr> calls;
    std::vector<u64> ids;
    std::vector<u64> refs;
    mgr.set_guest_call(
        [&](GuestAddr proc, const u64* args, usize count, bool is_64bit) -> u64 {
            EXPECT_TRUE(is_64bit);
            calls.push_back(proc);
            if (proc == BASE_WNDPROC) {
                EXPECT_EQ(count, 4u);
                return 0x55ull;
            }
            EXPECT_EQ(count, 6u);
            ids.push_back(args[4]);
            refs.push_back(args[5]);
            return call_def_subclass_proc(
                static_cast<windowing::HWND>(args[0]),
                static_cast<u32>(args[1]), args[2], args[3]);
        });

    EXPECT_EQ(
        mgr.dispatch_window_message(
            hwnd, 0x1234, 0x56, 0x78, BASE_WNDPROC, true),
        0x55u);
    EXPECT_EQ(
        calls,
        (std::vector<GuestAddr>{newer_proc, older_proc, BASE_WNDPROC}));
    EXPECT_EQ(ids, (std::vector<u64>{20, 10}));
    EXPECT_EQ(refs, (std::vector<u64>{0xBBBB, 0xAAAA}));
}

TEST_F(ComCtl32Test, RemovalDuringDispatchSkipsPendingCallback) {
    const auto hwnd = create_window();
    constexpr GuestAddr older_proc = 0x402000;
    constexpr GuestAddr newer_proc = 0x403000;
    ASSERT_TRUE(mgr.set_window_subclass(hwnd, older_proc, 10, 0));
    ASSERT_TRUE(mgr.set_window_subclass(hwnd, newer_proc, 20, 0));

    std::vector<GuestAddr> calls;
    mgr.set_guest_call(
        [&](GuestAddr proc, const u64* args, usize, bool) -> u64 {
            calls.push_back(proc);
            if (proc == newer_proc) {
                EXPECT_TRUE(
                    mgr.remove_window_subclass(hwnd, older_proc, 10));
                return call_def_subclass_proc(
                    static_cast<windowing::HWND>(args[0]),
                    static_cast<u32>(args[1]), args[2], args[3]);
            }
            return 0x99ull;
        });

    EXPECT_EQ(
        mgr.dispatch_window_message(
            hwnd, 0x1234, 0, 0, BASE_WNDPROC, true),
        0x99u);
    EXPECT_EQ(
        calls, (std::vector<GuestAddr>{newer_proc, BASE_WNDPROC}));
}

TEST_F(ComCtl32Test, NcDestroyDropsSubclassState) {
    const auto hwnd = create_window();
    constexpr GuestAddr proc = 0x402000;
    ASSERT_TRUE(mgr.set_window_subclass(hwnd, proc, 1, 0x1234));
    mgr.set_guest_call(
        [&](GuestAddr called, const u64*, usize, bool) -> u64 {
            return called == proc ? 1ull : 0ull;
        });

    (void)mgr.dispatch_window_message(
        hwnd, 0x0082, 0, 0, BASE_WNDPROC, true);

    u64 ref_data = 0;
    EXPECT_FALSE(mgr.get_window_subclass(hwnd, proc, 1, ref_data));
}

// =============================================================================
// Handle Validity Tests
// =============================================================================

TEST_F(ComCtl32Test, ImageListHandleIsNonZero) {
    cpu.gpr.rcx = 16;
    cpu.gpr.rdx = 16;

    mgr.impl_ImageList_Create(marshaler);
    u64 handle = cpu.gpr.rax;

    // Valid handle should be non-zero
    EXPECT_NE(handle, 0u);

    // Verify it's the expected stub handle
    EXPECT_EQ(handle, 0xABCD0001u);
}

TEST_F(ComCtl32Test, ImageListHandleHasMarkerBits) {
    cpu.gpr.rcx = 16;
    cpu.gpr.rdx = 16;

    mgr.impl_ImageList_Create(marshaler);
    u64 handle = cpu.gpr.rax;

    // Handle should have recognizable prefix (0xABCD)
    EXPECT_EQ((handle >> 16) & 0xFFFF, 0xABCDu);
}

// =============================================================================
// 32-bit Calling Convention Tests (stdcall)
// =============================================================================

class ComCtl32Test32 : public ::testing::Test {
protected:
    void SetUp() override {
        auto res = memory.map_region(DATA_BASE, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());

        auto stack_res = memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());

        // Set up 32-bit protected mode
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;

        marshaler.attach_cpu(cpu);
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE = 0x1000;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    ComCtl32Manager mgr;
};

TEST_F(ComCtl32Test32, InitCommonControlsExReturnsTrue32) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_WIN95_CLASSES;

    constexpr GuestAddr icc_addr = DATA_BASE + 0x100;
    (void)memory.write_bytes(icc_addr, reinterpret_cast<const Byte*>(&icc), sizeof(icc));

    // In 32-bit, args are on stack: [esp+4] = first arg
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), icc_addr);

    mgr.impl_InitCommonControlsEx(marshaler);

    // Check return in EAX (low 32 bits of RAX)
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

TEST_F(ComCtl32Test32, ImageListCreate32ReturnsHandle) {
    // Stack layout for ImageList_Create(cx, cy, flags, cInitial, cGrow):
    // [esp+4]  = cx
    // [esp+8]  = cy
    // [esp+12] = flags
    // [esp+16] = cInitial
    // [esp+20] = cGrow
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 16);     // cx
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 16);     // cy
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 0x21);  // flags
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 10);    // cInitial
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), 5);     // cGrow

    mgr.impl_ImageList_Create(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xABCD0001u);
}

TEST_F(ComCtl32Test, TrackMouseEventDelegatesGuestStructureAndReturnValue) {
    u32 calls = 0;
    const auto exports = create_comctl32_thunks(mgr, [&](Marshaler& m) {
        EXPECT_EQ(m.arg_ptr(0), DATA_BASE);
        ++calls;
        m.set_return(0);
    });
    cpu.gpr.rcx = DATA_BASE;
    bool found = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "_TrackMouseEvent") {
            found = true;
            EXPECT_EQ(entry.stdcall_bytes, 4u);
            entry.handler(marshaler);
            EXPECT_EQ(cpu.gpr.rax, 0u);
        }
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(calls, 1u);
}

TEST_F(ComCtl32Test, ScaleDownUnsupportedHasCorrectOrdinalAndX86StackSize) {
    auto exports = create_comctl32_thunks(mgr);
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "LoadIconWithScaleDown"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->ordinal, 381u);
    EXPECT_EQ(it->stdcall_bytes, 20u);
    for (bool mode64 : {false, true}) {
        if (mode64) cpu.init_long_mode(); else cpu.init_protected_mode();
        it->handler(marshaler);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80004001u);
    }
}

// Learn declares four WINAPI arguments and a BOOL failure value of zero.
// Flat-scroll state and rendering remain unsupported; neither bitness may
// report success or change caller-owned arguments/last-error on that path.
TEST_F(ComCtl32Test, FlatScrollPropertyUnsupportedPreservesAbiAndState) {
    auto exports = create_comctl32_thunks(mgr);
    const auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "FlatSB_SetScrollProp"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 4 * sizeof(u32));
    testing::internal::CaptureStderr();
    for (const bool mode64 : {false, true}) {
        if (mode64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        cpu.gpr.rcx = 0x1234;
        cpu.gpr.rdx = 0xffffffffu;
        cpu.gpr.r8 = 0x1234567887654321ull;
        cpu.gpr.r9 = 1;
        constexpr u32 arguments[] = {0x1234, 0xffffffffu, 0x76543210, 1};
        ASSERT_TRUE(memory.write_bytes(cpu.gpr.rsp + sizeof(u32),
                                      reinterpret_cast<const Byte*>(arguments), sizeof(arguments)).is_ok());
        const u64 stack = cpu.gpr.rsp;
        set_last_error(0x1234);
        it->handler(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(get_last_error(), 0x1234u);
        // The common thunk dispatcher consumes stdcall_bytes; the body must
        // not also pop the arguments or touch their storage.
        EXPECT_EQ(cpu.gpr.rsp, stack);
        u32 after[4]{};
        EXPECT_TRUE(memory.read_bytes(stack + sizeof(u32), reinterpret_cast<Byte*>(after), sizeof(after)).is_ok());
        EXPECT_TRUE(std::equal(std::begin(arguments), std::end(arguments), after));
    }
    const std::string diagnostic = testing::internal::GetCapturedStderr();
    const std::string prefix = "[vitreo] UNIMPLEMENTED comctl32!FlatSB_SetScrollProp:";
    const auto first = diagnostic.find(prefix);
    EXPECT_NE(first, std::string::npos);
    if (first != std::string::npos) EXPECT_EQ(diagnostic.find(prefix, first + 1), std::string::npos);
}

TEST_F(ComCtl32Test, FlatScrollInfoForwardsAllArgumentsAndResult) {
    for (bool mode64 : {false, true}) {
        if (mode64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        for (u32 bar : {0u, 1u, 2u, 0xffffffffu}) {
            for (u32 redraw : {0u, 1u}) {
                const GuestAddr info = mode64 ? 0x1234567887654321ull : DATA_BASE;
                cpu.gpr.rcx = 0x1234;
                cpu.gpr.rdx = bar;
                cpu.gpr.r8 = info;
                cpu.gpr.r9 = redraw;
                (void)memory.write_u32(cpu.gpr.rsp + 4, 0x1234);
                (void)memory.write_u32(cpu.gpr.rsp + 8, bar);
                (void)memory.write_u32(cpu.gpr.rsp + 12, static_cast<u32>(info));
                (void)memory.write_u32(cpu.gpr.rsp + 16, redraw);
                u32 calls = 0;
                const auto exports = create_comctl32_thunks(mgr, {}, [&](Marshaler& m) {
                    EXPECT_EQ(m.arg(0), 0x1234u);
                    EXPECT_EQ(static_cast<u32>(m.arg(1)), bar);
                    EXPECT_EQ(m.arg_ptr(2), info);
                    EXPECT_EQ(m.arg(3), redraw);
                    ++calls;
                    set_last_error(0x4321);
                    m.set_return(73);
                });
                const auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                    [](const ExportEntry& e) { return e.name == "FlatSB_SetScrollInfo"; });
                ASSERT_NE(it, exports.entries.end());
                EXPECT_EQ(it->stdcall_bytes, 4 * sizeof(u32));
                it->handler(marshaler);
                EXPECT_EQ(calls, 1u);
                EXPECT_EQ(cpu.gpr.rax, 73u);
                EXPECT_EQ(get_last_error(), 0x4321u);
            }
        }
    }
}

TEST_F(ComCtl32Test, FlatScrollFamilyForwardsExactAbi) {
    struct Case { const char* name; u32 bytes; ApiHandler FlatScrollHandlers::*member; };
    const Case cases[] = {
        {sdk_creation::FlatSB_GetScrollPosName, sdk_creation::FlatSB_GetScrollPosStackBytes, &FlatScrollHandlers::get_pos},
        {sdk_creation::FlatSB_SetScrollPosName, sdk_creation::FlatSB_SetScrollPosStackBytes, &FlatScrollHandlers::set_pos},
        {sdk_creation::FlatSB_GetScrollInfoName, sdk_creation::FlatSB_GetScrollInfoStackBytes, &FlatScrollHandlers::get_info},
        {sdk_creation::FlatSB_GetScrollRangeName, sdk_creation::FlatSB_GetScrollRangeStackBytes, &FlatScrollHandlers::get_range},
        {sdk_creation::FlatSB_SetScrollRangeName, sdk_creation::FlatSB_SetScrollRangeStackBytes, &FlatScrollHandlers::set_range},
        {sdk_creation::FlatSB_EnableScrollBarName, sdk_creation::FlatSB_EnableScrollBarStackBytes, &FlatScrollHandlers::enable},
        {sdk_creation::FlatSB_ShowScrollBarName, sdk_creation::FlatSB_ShowScrollBarStackBytes, &FlatScrollHandlers::show},
    };
    for (bool mode64 : {false, true}) {
        if (mode64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        const u64 args[] = {0x1234, 1, mode64 ? 0x1234567887654321ull : DATA_BASE, 0, 1};
        cpu.gpr.rcx = args[0]; cpu.gpr.rdx = args[1];
        cpu.gpr.r8 = args[2]; cpu.gpr.r9 = args[3];
        for (u32 i = 0; i < 5; ++i)
            (void)memory.write_u32(cpu.gpr.rsp + 4 + 4 * i, args[i]);
        (void)memory.write_u64(cpu.gpr.rsp + 40, args[4]);
        for (const auto& c : cases) {
            FlatScrollHandlers handlers;
            unsigned calls = 0;
            handlers.*c.member = [&](Marshaler& m) {
                for (u32 i = 0; i < c.bytes / sizeof(u32); ++i) EXPECT_EQ(m.arg(i), args[i]);
                ++calls;
                set_last_error(0x4321);
                m.set_return(0xfffffffdu);
            };
            const auto exports = create_comctl32_thunks(mgr, {}, {}, handlers);
            const auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                [&](const ExportEntry& e) { return e.name == c.name; });
            ASSERT_NE(it, exports.entries.end());
            EXPECT_EQ(it->stdcall_bytes, c.bytes);
            it->handler(marshaler);
            EXPECT_EQ(calls, 1u);
            EXPECT_EQ(cpu.gpr.rax, 0xfffffffdu);
            EXPECT_EQ(get_last_error(), 0x4321u);
        }
    }
}

// UninitializeFlatSB on a window that was never flat-initialized is S_FALSE.
// GetScrollProp and GetScrollPropPtr report that same uninitialized state as
// FALSE and must not write the caller output or invent a last-error.
TEST_F(ComCtl32Test, FlatScrollUninitAndGetPropMatchUninitializedContract) {
    auto exports = create_comctl32_thunks(mgr);
    const auto uninit = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == sdk_creation::UninitializeFlatSBName; });
    ASSERT_NE(uninit, exports.entries.end());
    EXPECT_EQ(uninit->stdcall_bytes, sdk_creation::UninitializeFlatSBStackBytes);
    const char* prop_names[] = {
        sdk_creation::FlatSB_GetScrollPropName,
        sdk_creation::FlatSB_GetScrollPropPtrName,
    };
    const u32 prop_bytes[] = {
        sdk_creation::FlatSB_GetScrollPropStackBytes,
        sdk_creation::FlatSB_GetScrollPropPtrStackBytes,
    };
    testing::internal::CaptureStderr();
    for (const bool mode64 : {false, true}) {
        if (mode64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_last_error(0x1234);
        uninit->handler(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0x00000001u);
        EXPECT_EQ(get_last_error(), 0x1234u);

        (void)memory.write_u32(DATA_BASE, 0xA5A5A5A5u);
        (void)memory.write_u32(DATA_BASE + 4, 0xA5A5A5A5u);
        for (usize i = 0; i < 2; ++i) {
            const auto prop = std::find_if(exports.entries.begin(), exports.entries.end(),
                [&](const ExportEntry& e) { return e.name == prop_names[i]; });
            ASSERT_NE(prop, exports.entries.end());
            EXPECT_EQ(prop->stdcall_bytes, prop_bytes[i]);
            cpu.gpr.rcx = 0x1234;
            cpu.gpr.rdx = 1;
            cpu.gpr.r8 = DATA_BASE;
            set_last_error(0x1234);
            prop->handler(marshaler);
            EXPECT_EQ(cpu.gpr.rax, 0u);
            EXPECT_EQ(get_last_error(), 0x1234u);
            EXPECT_EQ(memory.read_u32(DATA_BASE).value(), 0xA5A5A5A5u);
            EXPECT_EQ(memory.read_u32(DATA_BASE + 4).value(), 0xA5A5A5A5u);
        }
    }
    const std::string diagnostic = testing::internal::GetCapturedStderr();
    const std::string prefix = "[vitreo] UNIMPLEMENTED comctl32!FlatSB_GetScrollProp:";
    const auto first = diagnostic.find(prefix);
    EXPECT_NE(first, std::string::npos);
    if (first != std::string::npos) EXPECT_EQ(diagnostic.find(prefix, first + 1), std::string::npos);
}

// Property sheets: no frame exists, so no page handle is handed out. Size
// rule and untouched last error from tools/oracle_probe/
// propsheet_theme_session_probe{,.x86}.expected (V1 page/header size is 40 on
// x86, 72 on x64; smaller is refused before anything else).
TEST_F(ComCtl32Test, PropertySheetEntryPointsFailWithDocumentedValuesBothBitnesses) {
    DllExports exports = create_comctl32_thunks(mgr);
    auto find = [&](const char* name) -> const ExportEntry* {
        for (const auto& e : exports.entries) {
            if (e.name == name) return &e;
        }
        return nullptr;
    };
    const char* pages[] = {"CreatePropertySheetPageW", "CreatePropertySheetPageA",
                           "CreatePropertySheetPage"};
    const char* sheets[] = {"PropertySheetW", "PropertySheetA", "PropertySheet"};
    const ExportEntry* destroy = find("DestroyPropertySheetPage");
    ASSERT_NE(destroy, nullptr);
    EXPECT_EQ(destroy->stdcall_bytes, 4u);
    constexpr GuestAddr kStruct = DATA_BASE + 0x800;

    for (bool is64 : {false, true}) {
        cpu = cpu::CpuState{};
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        const u32 v1 = is64 ? 72u : 40u;
        auto call = [&](const ExportEntry* e, GuestAddr arg) {
            if (is64) cpu.gpr.rcx = arg;
            else ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, static_cast<u32>(arg)).is_ok());
            set_last_error(0xDEAD);
            e->handler(marshaler);
        };
        for (u32 size : {0u, v1 - 1, v1}) {
            ASSERT_TRUE(memory.write_u32(kStruct, size).is_ok());
            for (const char* name : pages) {
                const ExportEntry* e = find(name);
                ASSERT_NE(e, nullptr) << name;
                EXPECT_EQ(e->ordinal, 0u) << name;
                EXPECT_EQ(e->stdcall_bytes, 4u) << name;
                call(e, kStruct);
                EXPECT_EQ(cpu.gpr.rax, 0u) << name << " size " << size;
                EXPECT_EQ(get_last_error(), 0xDEADu) << name;
            }
            for (const char* name : sheets) {
                const ExportEntry* e = find(name);
                ASSERT_NE(e, nullptr) << name;
                EXPECT_EQ(e->ordinal, 0u) << name;
                EXPECT_EQ(e->stdcall_bytes, 4u) << name;
                call(e, kStruct);
                const u64 minus_one = is64 ? ~0ull : 0xFFFFFFFFull;
                EXPECT_EQ(cpu.gpr.rax & (is64 ? ~0ull : 0xFFFFFFFFull), minus_one)
                    << name << " size " << size;
                EXPECT_EQ(get_last_error(), 0xDEADu) << name;
            }
        }
        call(destroy, 0x1234);
        EXPECT_EQ(cpu.gpr.rax, 0u);
    }
}
