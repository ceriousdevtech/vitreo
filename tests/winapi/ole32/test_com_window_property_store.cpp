// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_window_property_store.cpp
/// @brief SHGetPropertyStoreForWindow's IPropertyStore against Windows.
///
/// Every expected result is a line of
/// tools/oracle_probe/window_propstore_probe32.expected (the x64 file is
/// identical apart from pointer and PROPVARIANT sizes); the quoted tags name
/// the line.

#include "com_test_common.hpp"
#include "com_test_shell_link_fixture.hpp"

#include "ipc/hwnd_registry.hpp"

#include <set>
#include <string>

namespace {

using WindowPropertyStoreTest = ShellLinkEncodingTest;

constexpr Guid kIidPropertyStore{
    0x886D8EEBu, 0x8CF2u, 0x4446u, {0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99}};
constexpr Guid kIidUnknown{
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid kIidCapabilities{
    0xC8E2D566u, 0x186Eu, 0x4D49u, {0xBF, 0x41, 0x69, 0x09, 0xEA, 0xD5, 0x6A, 0xCC}};
constexpr Guid kAumFmtid{
    0x9F4C2855u, 0x9F79u, 0x4B39u, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}};
constexpr Guid kTitleFmtid{
    0xF29F85E0u, 0x4FF9u, 0x1068u, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}};
constexpr Guid kOtherFmtid{
    0x12345678u, 0x1234u, 0x5678u, {1, 2, 3, 4, 5, 6, 7, 8}};
constexpr Guid kRandomClsid{
    0x0BADF00Du, 0x1111u, 0x2222u, {3, 3, 4, 4, 5, 5, 6, 6}};

constexpr u32 S_FALSE_HR = 1;
constexpr u32 E_FAIL_HR = 0x80004005u;
constexpr u32 E_NOINTERFACE_HR = 0x80004002u;
constexpr u32 INPLACE_S_TRUNCATED = 0x000401A0u;
constexpr u32 TYPE_E_TYPEMISMATCH = 0x80028CA0u;
constexpr u32 ERROR_INVALID_NAME_HR = 0x8007007Bu;
constexpr u32 ERROR_INSUFFICIENT_BUFFER_HR = 0x8007007Au;
constexpr u32 ERROR_INVALID_WINDOW_HANDLE_HR = 0x80070578u;

constexpr u32 kWindow = 0x00C0FFEEu;
constexpr u32 kOtherWindow = 0x00C0FFEFu;

class WindowStore : public WindowPropertyStoreTest {
protected:
    static constexpr GuestAddr KEY = DATA_BASE + 0x2000;
    static constexpr GuestAddr PV = DATA_BASE + 0x2040;
    static constexpr GuestAddr OUT_PV = DATA_BASE + 0x2080;
    static constexpr GuestAddr STR = DATA_BASE + 0x2100;
    static constexpr GuestAddr CLSID = DATA_BASE + 0x2400;
    static constexpr GuestAddr COUNT = DATA_BASE + 0x2420;
    static constexpr GuestAddr OUT_KEY = DATA_BASE + 0x2440;

    void SetUp() override {
        WindowPropertyStoreTest::SetUp();
        mgr_.set_window_exists([this](u32 hwnd) { return live_.contains(hwnd); });
        live_ = {kWindow, kOtherWindow};
        write_guid(IID_ADDR, kIidPropertyStore);
    }
    void TearDown() override {
        ipc::hwnd_registry_unpublish(kWindow);
        ipc::hwnd_registry_unpublish(kOtherWindow);
        WindowPropertyStoreTest::TearDown();
    }

    u32 open(u32 hwnd, const Guid& iid, u32& out) {
        write_guid(QI_IID_ADDR, iid);
        EXPECT_TRUE(memory_.write_u32(OUT_ADDR, 0x1234u).is_ok());
        auto m = make_marshaler();
        set_args({hwnd, QI_IID_ADDR, OUT_ADDR});
        const u32 hr = mgr_.create_window_property_store(m);
        out = read_u32(OUT_ADDR);
        return hr;
    }
    u32 store(u32 hwnd) {
        u32 out = 0;
        EXPECT_EQ(open(hwnd, kIidPropertyStore, out), com::S_OK);
        return out;
    }
    void key(const Guid& fmtid, u32 pid) {
        write_guid(KEY, fmtid);
        ASSERT_TRUE(memory_.write_u32(KEY + 16, pid).is_ok());
    }
    void pv(u16 vt, u64 value) {
        for (u32 off = 0; off < 16; off += 4) ASSERT_TRUE(memory_.write_u32(PV + off, 0).is_ok());
        ASSERT_TRUE(memory_.write_u16(PV, vt).is_ok());
        ASSERT_TRUE(memory_.write_u32(PV + 8, static_cast<u32>(value)).is_ok());
        ASSERT_TRUE(memory_.write_u32(PV + 12, static_cast<u32>(value >> 32)).is_ok());
    }
    void pv_text(const char* text) {
        auto m = make_marshaler();
        ASSERT_TRUE(m.write_wide_string(STR, text).is_ok());
        pv(31, STR);
    }
    u32 set(u32 ps) {
        auto m = make_marshaler();
        set_args({ps, KEY, PV});
        return mgr_.window_propstore_set_value(m);
    }
    u32 get(u32 ps) {
        for (u32 off = 0; off < 16; off += 4) (void)memory_.write_u32(OUT_PV + off, 0xCCCCCCCCu);
        auto m = make_marshaler();
        set_args({ps, KEY, OUT_PV});
        return mgr_.window_propstore_get_value(m);
    }
    u16 got_vt() { return memory_.read_u16(OUT_PV).value_or(0xFFFF); }
    std::string got_text() {
        auto m = make_marshaler();
        return m.read_wide_string(read_u32(OUT_PV + 8)).value_or("<unreadable>");
    }
    u32 count(u32 ps, u32& n) {
        EXPECT_TRUE(memory_.write_u32(COUNT, 0xDEADBEEFu).is_ok());
        auto m = make_marshaler();
        set_args({ps, COUNT});
        const u32 hr = mgr_.window_propstore_get_count(m);
        n = read_u32(COUNT);
        return hr;
    }
    u32 get_at(u32 ps, u32 index, Guid& fmtid, u32& pid) {
        for (u32 off = 0; off < 20; off += 4) (void)memory_.write_u32(OUT_KEY + off, 0xABABABABu);
        auto m = make_marshaler();
        set_args({ps, index, OUT_KEY});
        const u32 hr = mgr_.window_propstore_get_at(m);
        (void)memory_.read_bytes(OUT_KEY, reinterpret_cast<Byte*>(&fmtid), sizeof(fmtid));
        pid = read_u32(OUT_KEY + 16);
        return hr;
    }
    u32 release(u32 iface) {
        auto m = make_marshaler();
        set_args({iface});
        return mgr_.window_propstore_release(m);
    }

    std::set<u32> live_;
};

bool same_guid(const Guid& a, const Guid& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

TEST_F(WindowStore, CreationValidatesArgumentsAndInterfaces) {
    u32 out = 0;
    // "null-hwnd hr=0x80070057 ppv=untouched"
    EXPECT_EQ(open(0, kIidPropertyStore, out), com::E_INVALIDARG);
    EXPECT_EQ(out, 0x1234u);
    // "bogus-hwnd hr=0x00000000 ppv=set"
    EXPECT_EQ(open(0x7FF0ABCDu, kIidPropertyStore, out), com::S_OK);
    EXPECT_NE(out, 0x1234u);
    // "random-iid" / "capabilities-iid": E_NOINTERFACE, *ppv = NULL.
    EXPECT_EQ(open(kWindow, kRandomClsid, out), E_NOINTERFACE_HR);
    EXPECT_EQ(out, 0u);
    EXPECT_EQ(open(kWindow, kIidCapabilities, out), E_NOINTERFACE_HR);
    EXPECT_EQ(out, 0u);
    // "iunknown-iid hr=0x00000000 ppv=set"
    EXPECT_EQ(open(kWindow, kIidUnknown, out), com::S_OK);
    EXPECT_NE(out, 0u);
    // "null-ppv hr=0x80004003"
    auto m = make_marshaler();
    set_args({kWindow, QI_IID_ADDR, 0});
    EXPECT_EQ(mgr_.create_window_property_store(m), 0x80004003u);
}

TEST_F(WindowStore, IUnknownIsADistinctIdentityAndReferencesBalance) {
    const u32 ps = store(kWindow);
    write_guid(QI_IID_ADDR, kIidUnknown);
    auto m = make_marshaler();
    set_args({ps, QI_IID_ADDR, QI_OUT_ADDR});
    // "QI(IUnknown) hr=0x00000000 same=0"
    ASSERT_EQ(mgr_.window_propstore_query_interface(m), com::S_OK);
    const u32 unknown = read_u32(QI_OUT_ADDR);
    EXPECT_NE(unknown, ps);
    // "QI(Capabilities) hr=0x80004002"
    write_guid(QI_IID_ADDR, kIidCapabilities);
    set_args({ps, QI_IID_ADDR, QI_OUT_ADDR});
    EXPECT_EQ(mgr_.window_propstore_query_interface(m), E_NOINTERFACE_HR);
    EXPECT_EQ(read_u32(QI_OUT_ADDR), 0u);
    // "AddRef=2", "Release=1" (here 3 and 2: the QI above took one).
    set_args({ps});
    EXPECT_EQ(mgr_.window_propstore_add_ref(m), 3u);
    EXPECT_EQ(release(ps), 2u);
    EXPECT_EQ(release(unknown), 1u);
    EXPECT_EQ(release(ps), 0u);
    // A released store is gone.
    EXPECT_EQ(release(ps), 0u);
    u32 n = 0;
    EXPECT_EQ(count(ps, n), com::E_INVALIDARG);
}

TEST_F(WindowStore, ValuesLiveOnTheWindowAndConvertToText) {
    const u32 ps = store(kWindow);
    u32 n = 7;
    Guid fmtid{};
    u32 pid = 0;
    // "fresh GetCount hr=0x80004005 n=0", "fresh GetAt(0) hr=0x80004005"
    // with the key untouched, "fresh GetValue(AUMID) hr=0x00000000 vt=0".
    EXPECT_EQ(count(ps, n), E_FAIL_HR);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(get_at(ps, 0, fmtid, pid), E_FAIL_HR);
    EXPECT_EQ(pid, 0xABABABABu);
    key(kAumFmtid, 5);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_vt(), 0u);

    // "SetValue(AUMID,str)", "after-set ... n=1", GetAt(1) E_INVALIDARG.
    pv_text("Vitreo.Probe.Window");
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(count(ps, n), com::S_OK);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(get_at(ps, 0, fmtid, pid), com::S_OK);
    EXPECT_TRUE(same_guid(fmtid, kAumFmtid));
    EXPECT_EQ(pid, 5u);
    EXPECT_EQ(get_at(ps, 1, fmtid, pid), com::E_INVALIDARG);
    EXPECT_EQ(pid, 0xABABABABu);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_vt(), 31u);
    EXPECT_EQ(got_text(), "Vitreo.Probe.Window");

    // "store2 ... same-object=0" but the same value; "other GetCount ... n=0".
    const u32 ps2 = store(kWindow);
    EXPECT_NE(ps2, ps);
    EXPECT_EQ(get(ps2), com::S_OK);
    EXPECT_EQ(got_text(), "Vitreo.Probe.Window");
    const u32 other = store(kOtherWindow);
    EXPECT_EQ(count(other, n), E_FAIL_HR);

    // Conversions into the string-typed key: "after-ui4 ... str=\"7\"",
    // "after-bool ... \"-1\"", "after-i4 ... \"-5\"", "after-r8 ... \"1.5\"",
    // "after-bstr", "after-clsid ... {0BADF00D-1111-2222-0303-040405050606}".
    pv(19, 7);
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_text(), "7");
    pv(11, 0xFFFF);
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_text(), "-1");
    pv(3, static_cast<u32>(-5));
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_text(), "-5");
    pv(5, 0x3FF8000000000000ull);
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_text(), "1.5");
    {
        auto m = make_marshaler();
        ASSERT_TRUE(m.write_wide_string(STR, "Vitreo.Probe.Bstr").is_ok());
    }
    pv(8, STR);
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_text(), "Vitreo.Probe.Bstr");
    write_guid(CLSID, kRandomClsid);
    pv(72, CLSID);
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_text(), "{0BADF00D-1111-2222-0303-040405050606}");
    EXPECT_EQ(count(ps, n), com::S_OK);
    EXPECT_EQ(n, 1u);

    // "after-spaces": no validation of the text; 199 characters accepted.
    pv_text("Has Spaces In It");
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_text(), "Has Spaces In It");
    pv_text(std::string(199, 'a').c_str());
    EXPECT_EQ(set(ps), com::S_OK);

    // "after-padded ... hr=0x000401a0 str=\"Vitreo.Padded\"",
    // "after-tabs ... \"Tab.Id\"", "after-blank ... hr=0x000401a0 vt=0" with
    // the key still counted.
    pv_text("  Vitreo.Padded  ");
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), INPLACE_S_TRUNCATED);
    EXPECT_EQ(got_text(), "Vitreo.Padded");
    pv_text("\tTab.Id\r\n");
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), INPLACE_S_TRUNCATED);
    EXPECT_EQ(got_text(), "Tab.Id");
    pv_text("   ");
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), INPLACE_S_TRUNCATED);
    EXPECT_EQ(got_vt(), 0u);
    EXPECT_EQ(count(ps, n), com::S_OK);
    EXPECT_EQ(n, 1u);

    // "SetValue(AUMID,empty) hr=0x8007007b" removes the value.
    pv_text("");
    EXPECT_EQ(set(ps), ERROR_INVALID_NAME_HR);
    EXPECT_EQ(count(ps, n), E_FAIL_HR);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_vt(), 0u);
    // "SetValue(NULL key) hr=0x8007007a"
    auto m = make_marshaler();
    set_args({ps, 0, PV});
    EXPECT_EQ(mgr_.window_propstore_set_value(m), ERROR_INSUFFICIENT_BUFFER_HR);
    // "Commit hr=0x00000000"
    set_args({ps});
    EXPECT_EQ(mgr_.window_propstore_commit(m), com::S_OK);
}

TEST_F(WindowStore, BooleanAndUnknownKeysConvertOnRead) {
    const u32 ps = store(kWindow);
    key(kAumFmtid, 9);  // PreventPinning
    // "bool-text" lines: "0"/"false" false; "true", "-1", "2", " 1 " true.
    const struct { const char* text; bool value; } table[] = {
        {"0", false}, {"true", true}, {"false", false}, {"-1", true}, {"2", true},
    };
    for (const auto& row : table) {
        pv_text(row.text);
        EXPECT_EQ(set(ps), com::S_OK) << row.text;
        EXPECT_EQ(get(ps), com::S_OK) << row.text;
        EXPECT_EQ(got_vt(), 11u) << row.text;
        EXPECT_EQ(memory_.read_u16(OUT_PV + 8).value_or(7), row.value ? 0xFFFFu : 0u)
            << row.text;
    }
    pv_text(" 1 ");
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), INPLACE_S_TRUNCATED);  // see "after-padded"
    EXPECT_EQ(memory_.read_u16(OUT_PV + 8).value_or(7), 0xFFFFu);
    // "GetValue(PreventPinning,strnope) hr=0x80028ca0 vt=0"
    pv_text("nope");
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), TYPE_E_TYPEMISMATCH);
    EXPECT_EQ(got_vt(), 0u);
    // "GetValue(PreventPinning,ui4 0) ... bool=0"
    pv(19, 0);
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(memory_.read_u16(OUT_PV + 8).value_or(7), 0u);

    // "GetValue(Other,ui4) hr=0x00000001 vt=31 str=\"9\"",
    // "GetValue(Other,lead) hr=0x00000001 ... \" lead\"" (untrimmed), while
    // the described System.Title trims: "GetValue(Title,lead) hr=0x000401a0".
    key(kOtherFmtid, 2);
    pv(19, 9);
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), S_FALSE_HR);
    EXPECT_EQ(got_vt(), 31u);
    EXPECT_EQ(got_text(), "9");
    pv_text(" lead");
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), S_FALSE_HR);
    EXPECT_EQ(got_text(), " lead");
    key(kTitleFmtid, 2);
    pv_text(" lead");
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(get(ps), INPLACE_S_TRUNCATED);
    EXPECT_EQ(got_text(), "lead");
}

TEST_F(WindowStore, EmptyRemovesByMovingTheLastValue) {
    const u32 ps = store(kWindow);
    // "after-others": AUMID(5), PreventPinning(9), RelaunchCommand(2),
    // Title, Other in set order.
    key(kAumFmtid, 5);
    pv_text("Vitreo.Probe.Window");
    ASSERT_EQ(set(ps), com::S_OK);
    key(kAumFmtid, 9);
    pv(11, 0xFFFF);
    ASSERT_EQ(set(ps), com::S_OK);
    key(kAumFmtid, 2);
    pv_text("cmd.exe");
    ASSERT_EQ(set(ps), com::S_OK);
    key(kTitleFmtid, 2);
    pv_text("t");
    ASSERT_EQ(set(ps), com::S_OK);
    key(kOtherFmtid, 2);
    pv_text("t");
    ASSERT_EQ(set(ps), com::S_OK);
    // "SetValue(Title,unset,empty-vt) hr=0x00000000" for an absent key.
    key(kAumFmtid, 4);
    pv(0, 0);
    EXPECT_EQ(set(ps), com::S_OK);

    // "after-vt-empty": Other, PreventPinning, RelaunchCommand, Title.
    key(kAumFmtid, 5);
    pv(0, 0);
    EXPECT_EQ(set(ps), com::S_OK);
    u32 n = 0;
    EXPECT_EQ(count(ps, n), com::S_OK);
    ASSERT_EQ(n, 4u);
    Guid fmtid{};
    u32 pid = 0;
    const struct { const Guid* fmtid; u32 pid; } after_vt_empty[] = {
        {&kOtherFmtid, 2}, {&kAumFmtid, 9}, {&kAumFmtid, 2}, {&kTitleFmtid, 2}};
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_EQ(get_at(ps, i, fmtid, pid), com::S_OK);
        EXPECT_TRUE(same_guid(fmtid, *after_vt_empty[i].fmtid)) << i;
        EXPECT_EQ(pid, after_vt_empty[i].pid) << i;
    }
    // "after-all-empty": Other, Title.
    key(kAumFmtid, 9);
    EXPECT_EQ(set(ps), com::S_OK);
    key(kAumFmtid, 2);
    EXPECT_EQ(set(ps), com::S_OK);
    EXPECT_EQ(count(ps, n), com::S_OK);
    ASSERT_EQ(n, 2u);
    ASSERT_EQ(get_at(ps, 0, fmtid, pid), com::S_OK);
    EXPECT_TRUE(same_guid(fmtid, kOtherFmtid));
    ASSERT_EQ(get_at(ps, 1, fmtid, pid), com::S_OK);
    EXPECT_TRUE(same_guid(fmtid, kTitleFmtid));
}

TEST_F(WindowStore, ADestroyedWindowRefusesWritesAndHoldsNothing) {
    const u32 ps = store(kWindow);
    key(kAumFmtid, 5);
    pv_text("Vitreo.Probe.Window");
    ASSERT_EQ(set(ps), com::S_OK);
    // DestroyWindow: the window is gone and so are its values.
    live_.erase(kWindow);
    ipc::hwnd_registry_unpublish(kWindow);
    // "SetValue(AUMID,after-destroy) hr=0x80070578", "after-destroy GetCount
    // hr=0x80004005 n=0", GetValue S_OK VT_EMPTY.
    EXPECT_EQ(set(ps), ERROR_INVALID_WINDOW_HANDLE_HR);
    u32 n = 1;
    EXPECT_EQ(count(ps, n), E_FAIL_HR);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(get(ps), com::S_OK);
    EXPECT_EQ(got_vt(), 0u);
    // "bogus SetValue hr=0x80070578 ... Commit=0x00000000"
    const u32 bogus = store(0x7FF0ABCDu);
    EXPECT_EQ(set(bogus), ERROR_INVALID_WINDOW_HANDLE_HR);
    auto m = make_marshaler();
    set_args({bogus});
    EXPECT_EQ(mgr_.window_propstore_commit(m), com::S_OK);
}

} // namespace
