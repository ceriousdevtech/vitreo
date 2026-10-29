// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_file_dialog.cpp
/// @brief Common Item Dialog and IShellItem, through the ole32 method thunks.
///
/// Expected values are Windows' (tools/oracle_probe/file_dialog_probe
/// {32,64}.expected, identical on both bitnesses); the vtable order is the
/// SDK's (ShObjIdl_core.h).

#include "com_test_common.hpp"
#include "com_test_shell_link_fixture.hpp"
#include <winapi/com/ole32/shell_item_dialogs.hpp>

#include <fstream>

#define ASSERT_TRUE_RET(x) if (!(x)) { ADD_FAILURE() << #x; return 0; }

namespace {

constexpr Guid CLSID_FILEOPENDIALOG{0xDC1C5A9Cu, 0xE88Au, 0x4DDEu, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
constexpr Guid CLSID_FILESAVEDIALOG{0xC0B4E2F3u, 0xBA21u, 0x4773u, {0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B}};
constexpr Guid IID_IFILEOPENDIALOG{0xD57C7288u, 0xD4ADu, 0x4768u, {0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60}};
constexpr Guid IID_IFILEDIALOG{0x42F85136u, 0xDB7Eu, 0x439Cu, {0x85, 0xF1, 0xE4, 0x07, 0x5D, 0x13, 0x5F, 0xC8}};
constexpr Guid IID_IFILEDIALOGCUSTOMIZE{0xE6FDD21Au, 0x163Fu, 0x4975u, {0x9C, 0x8C, 0xA6, 0x9F, 0x1B, 0xA3, 0x70, 0x34}};
constexpr Guid IID_ISHELLITEM{0x43826D1Eu, 0xE718u, 0x42EEu, {0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE}};

constexpr u32 E_UNEXPECTED_HR = 0x8000FFFFu, E_FAIL_HR = 0x80004005u,
              E_INVALIDARG_HR = 0x80070057u, E_NOINTERFACE_HR = 0x80004002u,
              NOT_FOUND_HR = 0x80070002u;

class FileDialogTest : public ShellLinkEncodingTest {
protected:
    static constexpr GuestAddr TEXT_ADDR = DATA_BASE + 0x1000;
    static constexpr GuestAddr SPEC_ADDR = DATA_BASE + 0x2000;

    void SetUp() override {
        ShellLinkEncodingTest::SetUp();
        exports_ = create_com_core_thunks(mgr_);
        std::filesystem::create_directories(test_root_ + "/Windows/System32");
        std::ofstream(test_root_ + "/Windows/notepad.exe") << "MZ";
    }

    u32 call(const char* name, const std::vector<u32>& args) {
        set_args(args);
        auto m = make_marshaler();
        for (auto& entry : exports_.entries) {
            if (entry.name == name) {
                entry.handler(m);
                return static_cast<u32>(cpu_.gpr.rax);
            }
        }
        ADD_FAILURE() << "no thunk " << name;
        return 0xFFFFFFFFu;
    }

    void write_wide(GuestAddr at, std::string_view text) {
        for (usize i = 0; i < text.size(); ++i) {
            ASSERT_TRUE(memory_.write_u16(at + i * 2, static_cast<u8>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory_.write_u16(at + text.size() * 2, 0).is_ok());
    }

    std::string read_wide(GuestAddr at) {
        std::string out;
        for (GuestAddr i = 0; i < 1024; ++i) {
            const u16 c = memory_.read_u16(at + i * 2).value_or(0);
            if (c == 0) break;
            out.push_back(static_cast<char>(c));
        }
        return out;
    }

    u32 create(const Guid& clsid, const Guid& iid) {
        write_guid(CLSID_ADDR, clsid);
        write_guid(IID_ADDR, iid);
        set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
        auto m = make_marshaler();
        EXPECT_EQ(mgr_.co_create_instance(m), com::S_OK);
        return read_u32(OUT_ADDR);
    }

    u32 item(std::string_view path, u32* hr_out = nullptr) {
        write_wide(TEXT_ADDR, path);
        write_guid(IID_ADDR, IID_ISHELLITEM);
        ASSERT_TRUE_RET(memory_.write_u32(OUT_ADDR, 0xBAD).is_ok());
        set_args({TEXT_ADDR, 0, IID_ADDR, OUT_ADDR});
        auto m = make_marshaler();
        const u32 hr = mgr_.item_dialogs().create_item_from_parsing_name(m);
        if (hr_out) *hr_out = hr;
        return read_u32(OUT_ADDR);
    }

    std::string name_of(u32 shell_item, u32 sigdn, u32* hr_out = nullptr) {
        const u32 hr = call("Vitreo_ShellItem_GetDisplayName", {shell_item, sigdn, OUT_ADDR});
        if (hr_out) *hr_out = hr;
        return hr == 0 ? read_wide(read_u32(OUT_ADDR)) : std::string{};
    }

    DllExports exports_;
};

}  // namespace

TEST_F(FileDialogTest, OpenDialogVtableAndDefaultsMatchWindows) {
    const u32 dialog = create(CLSID_FILEOPENDIALOG, IID_IFILEOPENDIALOG);
    ASSERT_NE(dialog, 0u);
    const u32 vtable = read_u32(dialog);
    const struct { u32 slot; const char* name; } kSlots[] = {
        {0, "Vitreo_FileDialog_QueryInterface"}, {3, "Vitreo_FileDialog_Show"},
        {4, "Vitreo_FileDialog_SetFileTypes"}, {9, "Vitreo_FileDialog_SetOptions"},
        {20, "Vitreo_FileDialog_GetResult"}, {26, "Vitreo_FileDialog_SetFilter"},
        {27, "Vitreo_FileDialog_GetResults"}, {28, "Vitreo_FileDialog_GetSelectedItems"},
    };
    for (const auto& s : kSlots) {
        auto expected = registry_.get_thunk_address("ole32.dll", s.name);
        ASSERT_TRUE(expected.is_ok()) << s.name;
        EXPECT_EQ(read_u32(vtable + s.slot * 4), static_cast<u32>(expected.value())) << s.name;
    }
    const GuestAddr value = OUT_ADDR + 8;
    EXPECT_EQ(call("Vitreo_FileDialog_GetOptions", {dialog, value}), 0u);
    EXPECT_EQ(read_u32(value), 0x1808u);
    EXPECT_EQ(call("Vitreo_FileDialog_GetOptions", {dialog, 0}), E_INVALIDARG_HR);
    EXPECT_EQ(call("Vitreo_FileDialog_GetFileTypeIndex", {dialog, value}), 0u);
    EXPECT_EQ(read_u32(value), 0u);
    ASSERT_TRUE(memory_.write_u32(value, 0xBAD).is_ok());
    EXPECT_EQ(call("Vitreo_FileDialog_GetFileName", {dialog, value}), E_FAIL_HR);
    EXPECT_EQ(read_u32(value), 0u);
    ASSERT_TRUE(memory_.write_u32(value, 0xBAD).is_ok());
    EXPECT_EQ(call("Vitreo_FileDialog_GetResult", {dialog, value}), E_UNEXPECTED_HR);
    EXPECT_EQ(read_u32(value), 0xBADu) << "untouched";
    EXPECT_EQ(call("Vitreo_FileDialog_GetResults", {dialog, value}), E_FAIL_HR);
    EXPECT_EQ(read_u32(value), 0u);
    EXPECT_EQ(call("Vitreo_FileDialog_GetCurrentSelection", {dialog, value}), E_FAIL_HR);

    // COMDLG_FILTERSPEC[2] on x86: {name*, spec*} pairs.
    write_wide(TEXT_ADDR, "Text");
    write_wide(TEXT_ADDR + 0x40, "*.txt");
    write_wide(TEXT_ADDR + 0x80, "All");
    write_wide(TEXT_ADDR + 0xC0, "*.*");
    for (u32 i = 0; i < 4; ++i) ASSERT_TRUE(memory_.write_u32(SPEC_ADDR + i * 4, TEXT_ADDR + i * 0x40).is_ok());
    EXPECT_EQ(call("Vitreo_FileDialog_SetFileTypes", {dialog, 0, SPEC_ADDR}), 0u);
    EXPECT_EQ(call("Vitreo_FileDialog_SetFileTypes", {dialog, 2, 0}), E_INVALIDARG_HR);
    EXPECT_EQ(call("Vitreo_FileDialog_SetFileTypes", {dialog, 2, SPEC_ADDR}), 0u);
    EXPECT_EQ(call("Vitreo_FileDialog_SetFileTypes", {dialog, 2, SPEC_ADDR}), E_UNEXPECTED_HR);
    for (const auto& [set, expect] : {std::pair{0u, 1u}, std::pair{5u, 2u}, std::pair{2u, 2u}}) {
        EXPECT_EQ(call("Vitreo_FileDialog_SetFileTypeIndex", {dialog, set}), 0u);
        (void)call("Vitreo_FileDialog_GetFileTypeIndex", {dialog, value});
        EXPECT_EQ(read_u32(value), expect) << "set " << set;
    }
    EXPECT_EQ(call("Vitreo_FileDialog_SetOptions", {dialog, 0x20}), 0u);
    (void)call("Vitreo_FileDialog_GetOptions", {dialog, value});
    EXPECT_EQ(read_u32(value), 0x1020u) << "PICKFOLDERS adds FILEMUSTEXIST";
    write_wide(TEXT_ADDR, "a.txt");
    EXPECT_EQ(call("Vitreo_FileDialog_SetFileName", {dialog, TEXT_ADDR}), 0u);
    EXPECT_EQ(call("Vitreo_FileDialog_GetFileName", {dialog, value}), 0u);
    EXPECT_EQ(read_wide(read_u32(value)), "a.txt");
    EXPECT_EQ(call("Vitreo_FileDialog_Advise", {dialog, 0, value}), E_INVALIDARG_HR);
    EXPECT_EQ(call("Vitreo_FileDialog_Unadvise", {dialog, 12345}), E_INVALIDARG_HR);
    EXPECT_EQ(call("Vitreo_FileDialog_Close", {dialog, 0}), 0u);
    EXPECT_EQ(call("Vitreo_FileDialog_ClearClientData", {dialog}), E_FAIL_HR);
    write_guid(QI_IID_ADDR, IID_IFILEDIALOGCUSTOMIZE);
    EXPECT_EQ(call("Vitreo_FileDialog_QueryInterface", {dialog, QI_IID_ADDR, QI_OUT_ADDR}),
              E_NOINTERFACE_HR);
    write_guid(QI_IID_ADDR, IID_IFILEDIALOG);
    EXPECT_EQ(call("Vitreo_FileDialog_QueryInterface", {dialog, QI_IID_ADDR, QI_OUT_ADDR}), 0u);
    EXPECT_EQ(read_u32(QI_OUT_ADDR), dialog);
    EXPECT_EQ(call("Vitreo_FileDialog_Release", {dialog}), 1u);
    EXPECT_EQ(call("Vitreo_FileDialog_Release", {dialog}), 0u);
}

TEST_F(FileDialogTest, SaveDialogDefaultsAndRefusesPickFolders) {
    const u32 dialog = create(CLSID_FILESAVEDIALOG, IID_IFILEDIALOG);
    ASSERT_NE(dialog, 0u);
    const GuestAddr value = OUT_ADDR + 8;
    EXPECT_EQ(call("Vitreo_FileDialog_GetOptions", {dialog, value}), 0u);
    EXPECT_EQ(read_u32(value), 0x880Au);
    EXPECT_EQ(call("Vitreo_FileDialog_SetOptions", {dialog, 0x20}), E_INVALIDARG_HR);
    (void)call("Vitreo_FileDialog_GetOptions", {dialog, value});
    EXPECT_EQ(read_u32(value), 0x880Au);
    EXPECT_EQ(call("Vitreo_FileDialog_Release", {dialog}), 0u);
}

TEST_F(FileDialogTest, ShellItemNamesAttributesAndParents) {
    u32 hr = 0;
    const u32 file = item("c:\\windows\\NOTEPAD.EXE", &hr);
    ASSERT_EQ(hr, 0u);
    EXPECT_EQ(name_of(file, 0x80058000u), "C:\\Windows\\notepad.exe") << "canonical case";
    EXPECT_EQ(name_of(file, 0), "notepad.exe");
    EXPECT_EQ(name_of(file, 0x80068000u), "file:///C:/Windows/notepad.exe");
    EXPECT_EQ(name_of(file, 0x80028000u), "C:\\Windows\\notepad.exe");
    const GuestAddr attrs = OUT_ADDR + 8;
    // FOLDER|FILESYSTEM|FILESYSANCESTOR|STREAM|HIDDEN|READONLY|LINK|HASSUBFOLDER
    EXPECT_EQ(call("Vitreo_ShellItem_GetAttributes", {file, 0xF04D0000u, attrs}), 1u);
    EXPECT_EQ(read_u32(attrs), 0x40400000u);

    const u32 folder = item("C:\\Windows\\");
    EXPECT_EQ(name_of(folder, 0x80058000u), "C:\\Windows");
    EXPECT_EQ(call("Vitreo_ShellItem_GetAttributes", {folder, 0xF04D0000u, attrs}), 1u);
    EXPECT_EQ(read_u32(attrs), 0xF0000000u);
    EXPECT_EQ(call("Vitreo_ShellItem_GetAttributes", {folder, 0x40000000u, attrs}), 0u);

    EXPECT_EQ(call("Vitreo_ShellItem_GetParent", {file, OUT_ADDR + 12}), 0u);
    EXPECT_EQ(name_of(read_u32(OUT_ADDR + 12), 0x80058000u), "C:\\Windows");
    EXPECT_EQ(call("Vitreo_ShellItem_GetParent", {folder, OUT_ADDR + 12}), 0u);
    const u32 root = read_u32(OUT_ADDR + 12);
    EXPECT_EQ(name_of(root, 0x80058000u), "C:\\");
    EXPECT_EQ(name_of(root, 0x80018001u), "C:");
    EXPECT_EQ(name_of(root, 0x80068000u), "file:///C:/");

    // Compare: equal -> S_OK/0; an ancestor sorts first; folders before files.
    const u32 same = item("C:\\Windows");
    const u32 system32 = item("C:\\Windows\\System32");
    EXPECT_EQ(call("Vitreo_ShellItem_Compare", {folder, same, 0x10000000u, attrs}), 0u);
    EXPECT_EQ(read_u32(attrs), 0u);
    EXPECT_EQ(call("Vitreo_ShellItem_Compare", {folder, system32, 0x10000000u, attrs}), 1u);
    EXPECT_EQ(static_cast<i32>(read_u32(attrs)), -1);
    EXPECT_EQ(call("Vitreo_ShellItem_Compare", {file, system32, 0x10000000u, attrs}), 1u);
    EXPECT_EQ(static_cast<i32>(read_u32(attrs)), 1);

    for (u32 shell_item : {file, folder, root, same, system32}) {
        EXPECT_EQ(call("Vitreo_ShellItem_Release", {shell_item}), 0u);
    }
}

TEST_F(FileDialogTest, ParsingNameErrorsAndThisPc) {
    u32 hr = 0;
    EXPECT_EQ(item("C:\\Windows\\no_such_file_vitreo.txt", &hr), 0u);
    EXPECT_EQ(hr, NOT_FOUND_HR);
    EXPECT_EQ(item("C:\\no_such_dir_vitreo\\x.txt", &hr), 0u);
    EXPECT_EQ(hr, NOT_FOUND_HR);
    EXPECT_EQ(item("Windows", &hr), 0u);
    EXPECT_EQ(hr, NOT_FOUND_HR) << "relative names do not parse";

    const u32 this_pc = item("", &hr);
    ASSERT_EQ(hr, 0u);
    EXPECT_EQ(name_of(this_pc, 0), "This PC");
    EXPECT_EQ(name_of(this_pc, 0x80028000u), "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}");
    name_of(this_pc, 0x80058000u, &hr);
    EXPECT_EQ(hr, E_INVALIDARG_HR);
    EXPECT_EQ(call("Vitreo_ShellItem_GetAttributes", {this_pc, 0x40000000u, OUT_ADDR + 8}), 1u);
    EXPECT_EQ(read_u32(OUT_ADDR + 8), 0u);
}
