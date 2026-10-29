/// @file test_oledlg.cpp
/// @brief oledlg.dll: OleUIInsertObjectW/A and the export table.
///
/// Expected values come from the SDK (oledlg.h, OleDlg.lib x86/x64, the
/// oledlg.dll export table) and from tests/oracle_data/
/// oledlg_insertobject_probe{32,64}.expected (Windows 11, both bitnesses;
/// the probe is tools/oracle_probe/oledlg_insertobject_probe.c).
///
/// The dialog runs through the production path: every user32 / kernel32 /
/// ole32 / comdlg32 call oledlg makes goes through a ThunkRegistry to fake
/// exports registered here, which parse the DLGTEMPLATE oledlg builds and
/// keep a model of the dialog's controls. The fake DialogBoxIndirectParam
/// calls the dialog procedure thunk, then a per-test "user" script.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include <vitreo/common.hpp>
#include "capsule/registry/registry.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/ui/oledlg/oledlg.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::oledlg;

namespace {

constexpr GuestAddr kHeap = 0x00200000;
constexpr u32 kHeapSize = 0x00200000;
constexpr GuestAddr kStack = 0x00600000;
constexpr u32 kStackSize = 0x00100000;
constexpr GuestAddr kHook = 0x0BADF00D;
constexpr u32 kOwner = 0x00010010;
constexpr u32 kDialogHwnd = 0x00020020;
constexpr GuestAddr kModule = 0x00400000;
constexpr u32 kWmInitDialog = 0x0110;
constexpr u32 kWmCommand = 0x0111;
constexpr u32 kWmClose = 0x0010;

constexpr const char* kGood = "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0001}";

struct Ctl {
    u16 cls{0};
    u32 style{0};
    bool visible{false};
    bool enabled{true};
    bool exists{true};
    int check{0};
    std::string text;
    std::vector<std::string> items;
    int cursel{-1};
};

struct Dlg {
    std::string title;
    std::map<u32, Ctl> ctls;
    bool ended{false};
    u64 result{0};
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class OledlgTest : public ::testing::TestWithParam<bool> {
protected:
    bool x64() const { return GetParam(); }
    const InsertObjectLayout& L() const { return x64() ? kInsertObject64 : kInsertObject32; }

    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(kHeap, kHeapSize, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(kStack, kStackSize, MemoryProtection::ReadWrite).is_ok());
        thunks_ = std::make_unique<ThunkRegistry>(0x70000000);
        heap_next_ = kHeap + 0x1000;
        register_fakes();
        mgr_.set_thunk_registry(thunks_.get());
        mgr_.set_registry(&registry_);
        mgr_.set_guest_call([this](GuestAddr fn, const u64* args, usize n, bool) { return guest_call(fn, args, n); });
        ASSERT_TRUE(thunks_->register_dll("oledlg.dll", create_oledlg_thunks(mgr_)).is_ok());
        hook_ = [](u32, u64, u64) -> u64 { return 0; };
        populate_registry();
    }

    // --- the guest-call path -------------------------------------------
    u64 guest_call(GuestAddr fn, const u64* args, usize n) {
        if (fn == kHook) {
            hook_msgs_.push_back({static_cast<u32>(args[1]), args[2], args[3]});
            return hook_(static_cast<u32>(args[1]), args[2], args[3]);
        }
        auto handler = thunks_->resolve_handler(fn);
        if (handler.is_err()) {
            ADD_FAILURE() << "guest call to unknown address";
            return 0;
        }
        cpu::CpuState cpu;
        const GuestAddr sp = kStack + kStackSize - 0x8000 - static_cast<GuestAddr>(depth_++) * 0x2000;
        if (x64()) {
            cpu.init_long_mode();
            u64* regs[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < n; ++i) {
                if (i < 4) *regs[i] = args[i];
                (void)memory_.write_u64(sp + 8 + i * 8, args[i]);
            }
            cpu.gpr.rsp = sp;
        } else {
            cpu.init_protected_mode();
            for (usize i = 0; i < n; ++i) (void)memory_.write_u32(sp + 4 + i * 4, static_cast<u32>(args[i]));
            cpu.set_sp(sp);
        }
        Marshaler m(memory_);
        m.attach_cpu(cpu);
        const ApiHandler* entry = handler.value();
        (*entry)(m);
        --depth_;
        return x64() ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFu);
    }

    u32 call_oledlg(const char* name, std::vector<u64> args) {
        auto fn = thunks_->get_thunk_address("oledlg.dll", name);
        EXPECT_TRUE(fn.is_ok()) << name;
        return static_cast<u32>(guest_call(fn.value(), args.data(), args.size()));
    }

    // --- guest memory helpers --------------------------------------------
    GuestAddr alloc(u32 size) {
        const GuestAddr at = heap_next_;
        heap_next_ += (size + 15) & ~15u;
        for (u32 i = 0; i < size; ++i) (void)memory_.write_u8(at + i, 0);
        return at;
    }
    GuestAddr wstr(const std::string& s) {
        const GuestAddr at = alloc(static_cast<u32>(s.size() * 2 + 2));
        for (usize i = 0; i < s.size(); ++i) (void)memory_.write_u16(at + i * 2, static_cast<u8>(s[i]));
        return at;
    }
    GuestAddr astr(const std::string& s) {
        const GuestAddr at = alloc(static_cast<u32>(s.size() + 1));
        for (usize i = 0; i < s.size(); ++i) (void)memory_.write_u8(at + i, static_cast<u8>(s[i]));
        return at;
    }
    std::string read_w(GuestAddr at, usize max = 1024) {
        std::string s;
        for (usize i = 0; i < max; ++i) {
            const u16 c = memory_.read_u16(at + i * 2).value_or(0);
            if (c == 0) break;
            s.push_back(static_cast<char>(c));
        }
        return s;
    }
    std::string read_a(GuestAddr at, usize max = 1024) {
        std::string s;
        for (usize i = 0; i < max; ++i) {
            const u8 c = memory_.read_u8(at + i).value_or(0);
            if (c == 0) break;
            s.push_back(static_cast<char>(c));
        }
        return s;
    }
    void put_ptr(GuestAddr at, u64 v) {
        if (x64()) (void)memory_.write_u64(at, v);
        else (void)memory_.write_u32(at, static_cast<u32>(v));
    }
    u64 get_ptr(GuestAddr at) {
        return x64() ? memory_.read_u64(at).value_or(0) : memory_.read_u32(at).value_or(0);
    }
    std::array<u8, 16> guid(const char* text) { return *parse_guid(text); }
    void put_guid(GuestAddr at, const std::array<u8, 16>& g) {
        for (usize i = 0; i < 16; ++i) (void)memory_.write_u8(at + i, g[i]);
    }
    std::array<u8, 16> get_guid(GuestAddr at) {
        std::array<u8, 16> g{};
        for (usize i = 0; i < 16; ++i) g[i] = memory_.read_u8(at + i).value_or(0);
        return g;
    }

    /// An OLEUIINSERTOBJECT set up like the probe's init_io: cbStruct,
    /// owner, hook, lCustData 0x1234, a MAX_PATH "untouched" file buffer,
    /// clsid 0xAB.., sc 0x7777 and hMetaPict 0x5555 as sentinels.
    GuestAddr make_io(u32 flags = 0, bool wide = true) {
        const GuestAddr io = alloc(L().size);
        (void)memory_.write_u32(io, L().size);
        (void)memory_.write_u32(io + L().flags, flags);
        put_ptr(io + L().hwnd_owner, kOwner);
        put_ptr(io + L().hook, kHook);
        put_ptr(io + L().cust_data, 0x1234);
        file_ = wide ? alloc(kMaxPath * 2) : alloc(kMaxPath);
        if (wide) {
            const std::string u = "untouched";
            for (usize i = 0; i < u.size(); ++i) (void)memory_.write_u16(file_ + i * 2, static_cast<u8>(u[i]));
        } else {
            const std::string u = "untouched";
            for (usize i = 0; i < u.size(); ++i) (void)memory_.write_u8(file_ + i, static_cast<u8>(u[i]));
        }
        put_ptr(io + L().file, file_);
        (void)memory_.write_u32(io + L().cch_file, kMaxPath);
        for (u32 i = 0; i < 16; ++i) (void)memory_.write_u8(io + L().clsid + i, 0xAB);
        (void)memory_.write_u32(io + L().sc, 0x7777);
        put_ptr(io + L().hmeta_pict, 0x5555);
        return io;
    }
    bool clsid_untouched(GuestAddr io) { return get_guid(io + L().clsid) == std::array<u8, 16>{
        0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB}; }

    u32 insert(GuestAddr io, bool wide = true) {
        return call_oledlg(wide ? "OleUIInsertObjectW" : "OleUIInsertObjectA", {io});
    }

    // --- the user's actions ------------------------------------------------
    void command(u32 id, u32 code = 0) {
        const u64 args[4] = {kDialogHwnd, kWmCommand, (u64{code} << 16) | id, 0x30000u + id};
        (void)guest_call(dlgproc_, args, 4);
    }
    void message(u32 msg) {
        const u64 args[4] = {kDialogHwnd, msg, 0, 0};
        (void)guest_call(dlgproc_, args, 4);
    }
    Ctl& ctl(u32 id) { return dlg_.ctls[id]; }
    bool select_item(const char* clsid) {
        Ctl& list = ctl(kIdcObjectTypeList);
        for (usize i = 0; i < list.items.size(); ++i) {
            if (list.items[i].find(clsid) != std::string::npos) {
                list.cursel = static_cast<int>(i);
                command(kIdcObjectTypeList, 1);
                return true;
            }
        }
        return false;
    }
    u32 registered(const std::string& name) {
        auto it = messages_.find(name);
        return it == messages_.end() ? 0 : it->second;
    }
    bool hook_saw(u32 msg, u64 wp) {
        return std::any_of(hook_msgs_.begin(), hook_msgs_.end(),
                           [&](const auto& h) { return h.msg == msg && h.wp == wp; });
    }

    // --- registry ----------------------------------------------------------
    void set_key(const std::wstring& path, const wchar_t* value) {
        auto h = registry_.open_key(capsule::HKEY_CLASSES_ROOT, path, true);
        ASSERT_TRUE(h.is_ok());
        if (value) (void)registry_.set_value(h.value(), L"", capsule::RegValue{capsule::RegType::Sz, std::wstring(value)});
        registry_.close_key(h.value());
    }
    static std::wstring w(const char* s) { return std::wstring(s, s + std::strlen(s)); }
    void reg_cls(const char* clsid, const wchar_t* name, const wchar_t* sub, const wchar_t* server) {
        set_key(L"CLSID\\" + w(clsid), name);
        if (sub) set_key(L"CLSID\\" + w(clsid) + L"\\" + sub, nullptr);
        if (server) set_key(L"CLSID\\" + w(clsid) + L"\\LocalServer32", server);
    }
    void reg_prog(const wchar_t* progid, const wchar_t* name, const char* clsid, bool insertable,
                  const wchar_t* ole1, bool notins) {
        set_key(progid, name);
        if (clsid) set_key(std::wstring(progid) + L"\\CLSID", w(clsid).c_str());
        if (insertable) set_key(std::wstring(progid) + L"\\Insertable", nullptr);
        if (notins) set_key(std::wstring(progid) + L"\\NotInsertable", nullptr);
        if (ole1) set_key(std::wstring(progid) + L"\\protocol\\StdFileEditing\\server", ole1);
    }
    /// The probe's registrations (oledlg_insertobject_probe.c).
    void populate_registry() {
        const wchar_t* np = L"C:\\Windows\\System32\\notepad.exe";
        reg_cls(kGood, L"VitreoProbe Good", L"Insertable", np);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0002}", L"VitreoProbe NotInsertable", L"NotInsertable", np);
        set_key(L"CLSID\\{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0002}\\Insertable", nullptr);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0003}", L"VitreoProbe Excluded", L"Insertable", np);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0004}", L"VitreoProbe NoServer", L"Insertable", L"C:\\vitreo_missing\\nope.exe");
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0006}", L"VitreoProbe NoInsertableKey", nullptr, np);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0007}", nullptr, L"Insertable", np);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0005}", L"VitreoProbe ProgIdInsertable", nullptr, np);
        reg_prog(L"VitreoProbe.ProgOnly.1", L"VitreoProbe ProgOnly ProgID name", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0005}", true, nullptr, false);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0008}", L"VitreoProbe HkcuOnly", L"Insertable", np);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0009}", L"VitreoProbe Ole1Clsid", nullptr, nullptr);
        reg_prog(L"VitreoProbe.Ole1", L"VitreoProbe Ole1 ProgID name", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0009}", false, np, false);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000A}", L"VitreoProbe Inproc", L"Insertable", nullptr);
        set_key(L"CLSID\\{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000A}\\InprocServer32", L"C:\\Windows\\System32\\ole32.dll");
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000B}", L"VitreoProbe LocalServerNo32", L"Insertable", nullptr);
        set_key(L"CLSID\\{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000B}\\LocalServer", np);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000C}", L"VitreoProbe Control", L"Control", nullptr);
        reg_prog(L"VitreoProbe.Good.1", L"ProgName Good.1", kGood, true, nullptr, false);
        reg_prog(L"VitreoProbe.Good", L"ProgName Good", kGood, true, nullptr, false);
        reg_prog(L"VitreoProbe.NotIns.1", L"ProgName NotIns", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0002}", true, nullptr, false);
        reg_prog(L"VitreoProbe.NotInsP.1", L"ProgName NotInsP", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0006}", true, nullptr, true);
        reg_prog(L"VitreoProbe.Excl.1", L"ProgName Excl", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0003}", true, nullptr, false);
        reg_prog(L"VitreoProbe.NoSrv.1", L"ProgName NoSrv", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0004}", true, nullptr, false);
        reg_prog(L"VitreoProbe.NoName.1", L"ProgName NoName", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0007}", true, nullptr, false);
        reg_prog(L"VitreoProbe.NoClsidKey", L"ProgName NoClsidKey", nullptr, true, nullptr, false);
        reg_prog(L"VitreoProbe.Unreg.1", L"ProgName Unreg", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000F}", true, nullptr, false);
        reg_prog(L"VitreoProbe.Inproc.1", L"ProgName Inproc", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000A}", true, nullptr, false);
        reg_prog(L"VitreoProbe.LocalNo32.1", L"ProgName LocalNo32", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000B}", true, nullptr, false);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0010}", L"VitreoProbe Ole1b ClsidName", nullptr, nullptr);
        reg_prog(L"VitreoProbe.Ole1b", L"ProgName Ole1b", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0010}", false, L"C:\\vitreo_missing\\ole1.exe", false);
        reg_cls("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0011}", L"VitreoProbe Both ClsidName", nullptr, np);
        reg_prog(L"VitreoProbe.Both.1", L"ProgName Both", "{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0011}", true, np, false);
        files_ = {"c:\\windows\\system32\\notepad.exe", "c:\\windows\\system32\\ole32.dll", "c:\\windows\\win.ini"};
    }

    // --- fake exports --------------------------------------------------------
    void add(DllExports& e, const char* name, std::function<u64(Marshaler&)> fn) {
        e.entries.push_back(ExportEntry{name, 0, [fn](Marshaler& m) { m.set_return(fn(m)); }, 0});
    }
    u32 ctl_handle(u32 id) { return 0x00100000u + id; }
    std::optional<u32> ctl_id(u64 h) {
        if (h < 0x00100000u || h > 0x0010FFFFu) return std::nullopt;
        const u32 id = static_cast<u32>(h - 0x00100000u);
        auto it = dlg_.ctls.find(id);
        if (it == dlg_.ctls.end() || !it->second.exists) return std::nullopt;
        return id;
    }

    /// DLGTEMPLATE (non-EX) parser, enough for the templates in this test.
    void parse_template(GuestAddr t) {
        dlg_ = Dlg{};
        auto u16at = [&](GuestAddr a) { return memory_.read_u16(a).value_or(0); };
        GuestAddr p = t;
        const u32 style = memory_.read_u32(p).value_or(0);
        p += 8;
        const u16 cdit = u16at(p);
        p += 2 + 8;
        auto skip_sz_or_ord = [&](std::string* out) {
            if (u16at(p) == 0xFFFF) { p += 4; return; }
            std::string s;
            while (u16 c = u16at(p)) { s.push_back(static_cast<char>(c)); p += 2; }
            p += 2;
            if (out) *out = s;
        };
        skip_sz_or_ord(nullptr);  // menu
        skip_sz_or_ord(nullptr);  // class
        skip_sz_or_ord(&dlg_.title);
        if (style & 0x40) { p += 2; skip_sz_or_ord(nullptr); }
        for (u16 i = 0; i < cdit; ++i) {
            p = (p + 3) & ~GuestAddr{3};
            Ctl c;
            c.style = memory_.read_u32(p).value_or(0);
            p += 8 + 8;
            const u16 id = u16at(p);
            p += 2;
            if (u16at(p) == 0xFFFF) { c.cls = u16at(p + 2); p += 4; } else skip_sz_or_ord(nullptr);
            skip_sz_or_ord(&c.text);
            const u16 extra = u16at(p);
            p += 2 + extra;
            c.visible = (c.style & 0x10000000u) != 0;
            c.enabled = (c.style & 0x08000000u) == 0;
            dlg_.ctls[id] = c;
        }
    }

    void register_fakes() {
        DllExports u;
        add(u, "IsWindow", [this](Marshaler& m) -> u64 { return m.arg32(0) == kOwner || m.arg32(0) == kDialogHwnd; });
        auto dialog_box = [this](Marshaler& m) -> u64 {
            const GuestAddr t = m.arg_ptr(1);
            parse_template(t);
            dialog_instance_ = m.arg_ptr(0);
            dialog_owner_ = m.arg32(2);
            dlgproc_ = m.arg_ptr(3);
            const u64 init[4] = {kDialogHwnd, kWmInitDialog, 0, m.arg(4)};
            (void)guest_call(dlgproc_, init, 4);
            if (user_) user_();
            return dlg_.ended ? dlg_.result : static_cast<u64>(-1);
        };
        add(u, "DialogBoxIndirectParamW", dialog_box);
        add(u, "DialogBoxIndirectParamA", dialog_box);
        add(u, "EndDialog", [this](Marshaler& m) -> u64 {
            dlg_.ended = true;
            dlg_.result = m.arg(1);
            return 1;
        });
        add(u, "GetDlgItem", [this](Marshaler& m) -> u64 {
            auto it = dlg_.ctls.find(m.arg32(1));
            return it != dlg_.ctls.end() && it->second.exists ? ctl_handle(m.arg32(1)) : 0;
        });
        add(u, "ShowWindow", [this](Marshaler& m) -> u64 {
            if (auto id = ctl_id(m.arg(0))) { const bool was = ctl(*id).visible; ctl(*id).visible = m.arg32(1) != 0; return was; }
            return 0;
        });
        add(u, "EnableWindow", [this](Marshaler& m) -> u64 {
            if (auto id = ctl_id(m.arg(0))) { const bool was = !ctl(*id).enabled; ctl(*id).enabled = m.arg32(1) != 0; return was; }
            return 0;
        });
        add(u, "DestroyWindow", [this](Marshaler& m) -> u64 {
            if (auto id = ctl_id(m.arg(0))) { ctl(*id).exists = false; return 1; }
            return 0;
        });
        add(u, "SetWindowTextW", [this](Marshaler& m) -> u64 {
            if (m.arg32(0) == kDialogHwnd) dlg_.title = read_w(m.arg_ptr(1));
            return 1;
        });
        add(u, "SetDlgItemTextW", [this](Marshaler& m) -> u64 {
            auto it = dlg_.ctls.find(m.arg32(1));
            if (it == dlg_.ctls.end() || !it->second.exists) return 0;
            it->second.text = read_w(m.arg_ptr(2));
            return 1;
        });
        add(u, "GetDlgItemTextW", [this](Marshaler& m) -> u64 {
            auto it = dlg_.ctls.find(m.arg32(1));
            const GuestAddr buf = m.arg_ptr(2);
            const u32 cch = m.arg32(3);
            if (it == dlg_.ctls.end() || cch == 0) return 0;
            const std::string& s = it->second.text;
            const usize n = std::min<usize>(s.size(), cch - 1);
            for (usize i = 0; i < n; ++i) (void)memory_.write_u16(buf + i * 2, static_cast<u8>(s[i]));
            (void)memory_.write_u16(buf + n * 2, 0);
            return n;
        });
        add(u, "RegisterWindowMessageW", [this](Marshaler& m) -> u64 {
            const std::string name = read_w(m.arg_ptr(0));
            auto [it, fresh] = messages_.emplace(name, 0xC100u + static_cast<u32>(messages_.size()));
            (void)fresh;
            return it->second;
        });
        add(u, "PostMessageW", [this](Marshaler& m) -> u64 {
            posted_.push_back({m.arg32(0), m.arg32(1), m.arg(2), m.arg(3)});
            return 1;
        });
        add(u, "SendMessageW", [this](Marshaler& m) -> u64 {
            auto id = ctl_id(m.arg(0));
            if (!id) return 0;
            Ctl& c = ctl(*id);
            const u32 msg = m.arg32(1);
            const u64 wp = m.arg(2);
            const GuestAddr lp = m.arg_ptr(3);
            switch (msg) {
            case 0x00F0: return static_cast<u64>(c.check);
            case 0x00F1: c.check = static_cast<int>(wp); return 0;
            case 0x0184: c.items.clear(); c.cursel = -1; return 0;
            case 0x0180: {
                const std::string s = read_w(lp);
                auto pos = c.items.end();
                if (c.style & 0x2) {
                    pos = std::find_if(c.items.begin(), c.items.end(),
                                       [&](const std::string& e) { return lower(e) > lower(s); });
                }
                return static_cast<u64>(c.items.insert(pos, s) - c.items.begin());
            }
            case 0x0186: c.cursel = static_cast<int>(wp) < static_cast<int>(c.items.size()) ? static_cast<int>(wp) : -1; return 0;
            case 0x0188: return static_cast<u64>(static_cast<i64>(c.cursel)) & (x64() ? ~u64{0} : 0xFFFFFFFFu);
            case 0x018A: return wp < c.items.size() ? c.items[wp].size() : static_cast<u64>(-1);
            case 0x0189:
                if (wp >= c.items.size()) return static_cast<u64>(-1);
                for (usize i = 0; i < c.items[wp].size(); ++i) (void)memory_.write_u16(lp + i * 2, static_cast<u8>(c.items[wp][i]));
                (void)memory_.write_u16(lp + c.items[wp].size() * 2, 0);
                return c.items[wp].size();
            case 0x018B: return c.items.size();
            case 0x0192: return 1;
            default: return 0;
            }
        });
        ASSERT_TRUE(thunks_->register_dll("user32.dll", std::move(u)).is_ok());

        DllExports k;
        add(k, "LocalAlloc", [this](Marshaler& m) -> u64 { return alloc(static_cast<u32>(m.arg(1))); });
        add(k, "LocalFree", [](Marshaler&) -> u64 { return 0; });
        auto find_res = [this](Marshaler& m, bool wide) -> u64 {
            if (m.arg_ptr(0) != kModule || m.arg32(2) != 5) return 0;
            const GuestAddr name = m.arg_ptr(1);
            const std::string key = name <= 0xFFFF ? "#" + std::to_string(name) : (wide ? read_w(name) : read_a(name));
            auto it = resources_.find(key);
            return it == resources_.end() ? 0 : it->second;
        };
        add(k, "FindResourceW", [find_res](Marshaler& m) { return find_res(m, true); });
        add(k, "FindResourceA", [find_res](Marshaler& m) { return find_res(m, false); });
        add(k, "LoadResource", [](Marshaler& m) -> u64 { return m.arg(1); });
        add(k, "LockResource", [](Marshaler& m) -> u64 { return m.arg(0); });
        add(k, "GetFileAttributesW", [this](Marshaler& m) -> u64 {
            return files_.count(lower(read_w(m.arg_ptr(0)))) ? 0x20u : 0xFFFFFFFFu;
        });
        add(k, "ExpandEnvironmentStringsW", [](Marshaler&) -> u64 { return 0; });
        ASSERT_TRUE(thunks_->register_dll("kernel32.dll", std::move(k)).is_ok());

        DllExports c;
        add(c, "GetOpenFileNameW", [this](Marshaler& m) -> u64 {
            ofn_seen_ = m.arg_ptr(0);
            if (!open_file_) return 0;
            const GuestAddr buf = get_ptr(m.arg_ptr(0) + (x64() ? 48 : 28));
            const std::string s = *open_file_;
            for (usize i = 0; i < s.size(); ++i) (void)memory_.write_u16(buf + i * 2, static_cast<u8>(s[i]));
            (void)memory_.write_u16(buf + s.size() * 2, 0);
            return 1;
        });
        ASSERT_TRUE(thunks_->register_dll("comdlg32.dll", std::move(c)).is_ok());
    }

    void register_ole32(u32 create_hr, bool icon) {
        DllExports o;
        add(o, "OleCreate", [this, create_hr](Marshaler& m) -> u64 {
            created_clsid_ = get_guid(m.arg_ptr(0));
            const GuestAddr ppv = m.arg_ptr(6);
            put_ptr(ppv, create_hr & 0x80000000u ? 0 : 0x0DEC0DE0);
            return create_hr;
        });
        if (icon) add(o, "OleGetIconOfClass", [](Marshaler&) -> u64 { return 0x00ABC000; });
        ASSERT_TRUE(thunks_->register_dll("ole32.dll", std::move(o)).is_ok());
    }

    struct HookMsg { u32 msg; u64 wp; u64 lp; };
    struct Posted { u32 hwnd; u32 msg; u64 wp; u64 lp; };

    GuestMemory memory_;
    std::unique_ptr<ThunkRegistry> thunks_;
    capsule::Registry registry_;
    OledlgManager mgr_;
    GuestAddr heap_next_{0};
    int depth_{0};
    Dlg dlg_;
    GuestAddr dlgproc_{0};
    GuestAddr dialog_instance_{0};
    u32 dialog_owner_{0};
    GuestAddr file_{0};
    std::function<void()> user_;
    std::function<u64(u32, u64, u64)> hook_;
    std::vector<HookMsg> hook_msgs_;
    std::vector<Posted> posted_;
    std::map<std::string, u32> messages_;
    std::map<std::string, GuestAddr> resources_;
    std::set<std::string> files_;
    std::optional<std::string> open_file_;
    GuestAddr ofn_seen_{0};
    std::array<u8, 16> created_clsid_{};
};

// --- SDK facts -----------------------------------------------------------------

TEST(OledlgSdk, ExportTableMatchesImportLibAndDll) {
    // OleDlg.lib (10.0.26100.0) x86 decorations and the oledlg.dll export
    // table ordinals; the import lib links every A function and
    // OleUICanConvertOrActivateAs by ordinal.
    const std::vector<std::tuple<std::string, u16, u16>> sdk = {
        {"OleUIAddVerbMenuA", 1, 36}, {"OleUICanConvertOrActivateAs", 2, 12}, {"OleUIInsertObjectA", 3, 4},
        {"OleUIPasteSpecialA", 4, 4}, {"OleUIEditLinksA", 5, 4}, {"OleUIChangeIconA", 6, 4},
        {"OleUIConvertA", 7, 4}, {"OleUIBusyA", 8, 4}, {"OleUIUpdateLinksA", 9, 16},
        {"OleUIPromptUserA", 10, 0}, {"OleUIObjectPropertiesA", 11, 4}, {"OleUIChangeSourceA", 12, 4},
        {"OleUIAddVerbMenuW", 13, 36}, {"OleUIBusyW", 14, 4}, {"OleUIChangeIconW", 15, 4},
        {"OleUIChangeSourceW", 16, 4}, {"OleUIConvertW", 17, 4}, {"OleUIEditLinksW", 18, 4},
        {"OleUIInsertObjectW", 19, 4}, {"OleUIObjectPropertiesW", 20, 4}, {"OleUIPasteSpecialW", 21, 4},
        {"OleUIPromptUserW", 22, 0}, {"OleUIUpdateLinksW", 23, 16}};
    ASSERT_EQ(kExports.size(), sdk.size());
    for (usize i = 0; i < sdk.size(); ++i) {
        EXPECT_EQ(kExports[i].name, std::get<0>(sdk[i]));
        EXPECT_EQ(kExports[i].ordinal, std::get<1>(sdk[i]));
        EXPECT_EQ(kExports[i].stdcall_bytes, std::get<2>(sdk[i]));
    }
    ThunkRegistry reg(0x70000000);
    OledlgManager mgr;
    ASSERT_TRUE(reg.register_dll("oledlg.dll", create_oledlg_thunks(mgr)).is_ok());
    for (const auto& [name, ordinal, bytes] : sdk) {
        auto by_name = reg.get_thunk_address("oledlg.dll", name);
        auto by_ord = reg.get_thunk_address("oledlg.dll", ordinal);
        ASSERT_TRUE(by_name.is_ok()) << name;
        ASSERT_TRUE(by_ord.is_ok()) << name;
        EXPECT_EQ(by_name.value(), by_ord.value()) << name;
    }
    // The dialog procedure is internal: not a guest-visible export.
    EXPECT_TRUE(reg.get_thunk_address("oledlg.dll", "VitreoOleUIInsertObjectDlgProc").is_err());
}

TEST(OledlgSdk, InsertObjectLayoutMatchesProbe) {
    // oledlg_insertobject_probe .expected: "sizeof W=176 A=176" / "offsets:
    // clsid=64 lpszFile=80 cchFile=88 iid=104 oleRender=120 lpFormatEtc=128
    // ppvObj=152 sc=160 hMetaPict=168"; x86: 112, 36 52 56 68 84 88 100 104 108.
    EXPECT_EQ(kInsertObject64.size, 176u);
    EXPECT_EQ(kInsertObject64.clsid, 64u);
    EXPECT_EQ(kInsertObject64.file, 80u);
    EXPECT_EQ(kInsertObject64.cch_file, 88u);
    EXPECT_EQ(kInsertObject64.iid, 104u);
    EXPECT_EQ(kInsertObject64.ole_render, 120u);
    EXPECT_EQ(kInsertObject64.format_etc, 128u);
    EXPECT_EQ(kInsertObject64.ppv_obj, 152u);
    EXPECT_EQ(kInsertObject64.sc, 160u);
    EXPECT_EQ(kInsertObject64.hmeta_pict, 168u);
    EXPECT_EQ(kInsertObject32.size, 112u);
    EXPECT_EQ(kInsertObject32.clsid, 36u);
    EXPECT_EQ(kInsertObject32.file, 52u);
    EXPECT_EQ(kInsertObject32.cch_file, 56u);
    EXPECT_EQ(kInsertObject32.iid, 68u);
    EXPECT_EQ(kInsertObject32.ole_render, 84u);
    EXPECT_EQ(kInsertObject32.format_etc, 88u);
    EXPECT_EQ(kInsertObject32.ppv_obj, 100u);
    EXPECT_EQ(kInsertObject32.sc, 104u);
    EXPECT_EQ(kInsertObject32.hmeta_pict, 108u);
}

TEST(OledlgSdk, GuidRoundTrip) {
    const auto g = parse_guid("{F20DA720-C02F-11CE-927B-0800095AE340}");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ((*g)[0], 0x20);
    EXPECT_EQ((*g)[3], 0xF2);
    EXPECT_EQ((*g)[8], 0x92);
    EXPECT_EQ(guid_string(*g), "{F20DA720-C02F-11CE-927B-0800095AE340}");
    EXPECT_FALSE(parse_guid("F20DA720-C02F-11CE-927B-0800095AE340").has_value());
}

// --- validation (probe "== validation ==") -------------------------------------

TEST_P(OledlgTest, ValidationReturnCodes) {
    user_ = [this] { command(2); };
    EXPECT_EQ(insert(0), 101u);
    auto with = [&](auto mutate) {
        const GuestAddr io = make_io();
        mutate(io);
        const u32 r = insert(io);
        return std::make_pair(r, io);
    };
    for (u32 cb : {0u, L().size - 1, L().size + 4}) {
        auto [r, at] = with([&](GuestAddr io) { (void)memory_.write_u32(io, cb); });
        EXPECT_EQ(r, 103u) << cb;
        EXPECT_TRUE(clsid_untouched(at));
        EXPECT_EQ(memory_.read_u32(at + L().sc).value(), 0x7777u);
    }
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().hwnd_owner, 0x12340); }).first, 104u);
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().hwnd_owner, 0); }).first, 2u);
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().caption, 0x10); }).first, 2u);
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().template_name, wstr("NOPE")); }).first, 2u);
    EXPECT_EQ(with([&](GuestAddr io) {
        put_ptr(io + L().template_name, wstr("NOPE"));
        put_ptr(io + L().hinstance, kModule);
    }).first, 110u);
    EXPECT_EQ(with([&](GuestAddr io) {
        put_ptr(io + L().template_name, 77);
        put_ptr(io + L().hinstance, kModule);
    }).first, 110u);
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().hinstance, kModule); }).first, 108u);
    EXPECT_EQ(with([&](GuestAddr io) {
        put_ptr(io + L().hinstance, kModule);
        put_ptr(io + L().hresource, 0x5000);
    }).first, 108u);
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().hresource, 0x5000); }).first, 112u);
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().file, 0); }).first, 116u);
    EXPECT_EQ(with([&](GuestAddr io) { (void)memory_.write_u32(io + L().cch_file, 0); }).first, 125u);
    EXPECT_EQ(with([&](GuestAddr io) { (void)memory_.write_u32(io + L().cch_file, 1); }).first, 2u);
    EXPECT_EQ(with([&](GuestAddr io) { (void)memory_.write_u32(io + L().cch_file, kMaxPath + 1); }).first, 125u);
    EXPECT_EQ(with([&](GuestAddr io) { (void)memory_.write_u32(io + L().c_clsid_exclude, 2); }).first, 124u);
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().clsid_exclude, 0x10); }).first, 2u);
    // Creation pointers are not validated.
    EXPECT_EQ(with([&](GuestAddr io) { (void)memory_.write_u32(io + L().flags, kIofCreateNewObject); }).first, 2u);
    EXPECT_EQ(with([&](GuestAddr io) { (void)memory_.write_u32(io + L().flags, 0x80000000u); }).first, 2u);
    // Order (probe "order ..." lines).
    EXPECT_EQ(with([&](GuestAddr io) { (void)memory_.write_u32(io, 0); put_ptr(io + L().hwnd_owner, 0x12340); }).first, 103u);
    EXPECT_EQ(with([&](GuestAddr io) {
        put_ptr(io + L().template_name, wstr("NOPE"));
        put_ptr(io + L().hinstance, kModule);
        (void)memory_.write_u32(io + L().c_clsid_exclude, 2);
    }).first, 110u);
    EXPECT_EQ(with([&](GuestAddr io) {
        put_ptr(io + L().hinstance, kModule);
        (void)memory_.write_u32(io + L().c_clsid_exclude, 2);
    }).first, 108u);
    EXPECT_EQ(with([&](GuestAddr io) {
        put_ptr(io + L().template_name, wstr("NOPE"));
        put_ptr(io + L().hinstance, kModule);
        put_ptr(io + L().file, 0);
    }).first, 110u);
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().file, 0); (void)memory_.write_u32(io + L().c_clsid_exclude, 2); }).first, 124u);
    EXPECT_EQ(with([&](GuestAddr io) { (void)memory_.write_u32(io + L().cch_file, 0); (void)memory_.write_u32(io + L().c_clsid_exclude, 2); }).first, 124u);
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().file, 0); (void)memory_.write_u32(io + L().cch_file, 0); }).first, 116u);
    EXPECT_EQ(with([&](GuestAddr io) { put_ptr(io + L().caption, 0x10); put_ptr(io + L().file, 0); }).first, 116u);
}

TEST_P(OledlgTest, AnsiValidationAndSharedSize) {
    user_ = [this] { command(2); };
    EXPECT_EQ(insert(0, false), 101u);
    GuestAddr io = make_io(0, false);
    (void)memory_.write_u32(io, 0);
    EXPECT_EQ(insert(io, false), 103u);
    io = make_io(0, false);
    put_ptr(io + L().file, 0);
    EXPECT_EQ(insert(io, false), 116u);
    io = make_io(0, false);
    (void)memory_.write_u32(io + L().cch_file, 0);
    EXPECT_EQ(insert(io, false), 125u);
    io = make_io(0, false);
    put_ptr(io + L().hwnd_owner, 0x12340);
    EXPECT_EQ(insert(io, false), 104u);
}

// --- the dialog ------------------------------------------------------------------

TEST_P(OledlgTest, CreateNewListsInsertableProgIds) {
    std::vector<std::string> at_hook_init;
    hook_ = [&](u32 msg, u64, u64) -> u64 {
        if (msg == kWmInitDialog) at_hook_init = ctl(kIdcObjectTypeList).items;
        return 0;
    };
    user_ = [this] { command(2); };
    const GuestAddr io = make_io(kIofSelectCreateNew);
    EXPECT_EQ(insert(io), 2u);
    // Probe "settled plain" (the two system entries aside).
    const std::vector<std::string> expected = {
        "ProgName Ole1b\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0010}",
        "VitreoProbe Both ClsidName\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0011}",
        "VitreoProbe Excluded\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0003}",
        "VitreoProbe Good\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0001}",
        "VitreoProbe Inproc\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000A}",
        "VitreoProbe LocalServerNo32\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000B}",
        "VitreoProbe NoServer\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0004}",
        "VitreoProbe NotInsertable\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0002}",
        "VitreoProbe Ole1 ProgID name\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0009}",
        "VitreoProbe ProgIdInsertable\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0005}",
    };
    EXPECT_EQ(ctl(kIdcObjectTypeList).items, expected);
    // The hook's WM_INITDIALOG comes after the library filled the dialog,
    // with lParam = lCustData.
    EXPECT_EQ(at_hook_init, expected);
    ASSERT_FALSE(hook_msgs_.empty());
    EXPECT_EQ(hook_msgs_.front().msg, kWmInitDialog);
    EXPECT_EQ(hook_msgs_.front().lp, 0x1234u);
    EXPECT_EQ(ctl(kIdcObjectTypeList).cursel, 0);
    EXPECT_EQ(ctl(kIdcCreateNew).check, 1);
    EXPECT_EQ(ctl(kIdcCreateFromFile).check, 0);
    EXPECT_TRUE(ctl(1).enabled);
    EXPECT_TRUE(ctl(kIdcObjectTypeList).visible);
    EXPECT_FALSE(ctl(kIdcFileDisplay).visible);
    EXPECT_FALSE(ctl(kIdcLinkFile).visible);
    EXPECT_TRUE(ctl(kIdcDisplayAsIcon).visible);
    EXPECT_TRUE(ctl(kIdcDisplayAsIcon).enabled);
    EXPECT_FALSE(ctl(kIdcHelp).visible);
    EXPECT_FALSE(ctl(kIdcInsertControl).visible);
    EXPECT_EQ(dlg_.title, "Insert Object");
    EXPECT_EQ(dialog_owner_, kOwner);
    // Cancel writes nothing and tells the hook first.
    EXPECT_TRUE(hook_saw(registered("OLEUI_MSG_ENDDIALOG"), 2));
    EXPECT_TRUE(clsid_untouched(io));
    EXPECT_EQ(read_w(file_), "untouched");
    EXPECT_EQ(memory_.read_u32(io + L().sc).value(), 0x7777u);
    EXPECT_EQ(get_ptr(io + L().hmeta_pict), 0x5555u);
}

TEST_P(OledlgTest, ExcludeAndVerifyServers) {
    user_ = [this] { command(2); };
    GuestAddr io = make_io(kIofSelectCreateNew);
    const GuestAddr ex = alloc(16);
    put_guid(ex, guid("{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D0003}"));
    (void)memory_.write_u32(io + L().c_clsid_exclude, 1);
    put_ptr(io + L().clsid_exclude, ex);
    EXPECT_EQ(insert(io), 2u);
    EXPECT_EQ(ctl(kIdcObjectTypeList).items.size(), 9u);
    for (const auto& s : ctl(kIdcObjectTypeList).items) EXPECT_EQ(s.find("Excluded"), std::string::npos);

    io = make_io(kIofSelectCreateNew | kIofVerifyServersExist);
    EXPECT_EQ(insert(io), 2u);
    // Probe "settled verify": NoServer and the OLE 1 entry with a missing
    // server go; the in-process and LocalServer (no 32) ones stay.
    const auto& items = ctl(kIdcObjectTypeList).items;
    EXPECT_EQ(items.size(), 8u);
    for (const auto& s : items) {
        EXPECT_EQ(s.find("NoServer"), std::string::npos);
        EXPECT_EQ(s.find("ProgName Ole1b"), std::string::npos);
    }
}

TEST_P(OledlgTest, CreateNewOkWritesClsidOnly) {
    user_ = [this] {
        ASSERT_TRUE(select_item(kGood));
        command(1);
    };
    const GuestAddr io = make_io(kIofSelectCreateNew);
    EXPECT_EQ(insert(io), 1u);
    // Probe "settled select Good OK": clsid=Good, flags as passed, sc 0,
    // hMetaPict NULL, lpszFile untouched, ENDDIALOG(IDOK).
    EXPECT_EQ(get_guid(io + L().clsid), guid(kGood));
    EXPECT_EQ(memory_.read_u32(io + L().flags).value(), kIofSelectCreateNew);
    EXPECT_EQ(memory_.read_u32(io + L().sc).value(), 0u);
    EXPECT_EQ(get_ptr(io + L().hmeta_pict), 0u);
    EXPECT_EQ(read_w(file_), "untouched");
    EXPECT_TRUE(hook_saw(registered("OLEUI_MSG_ENDDIALOG"), 1));
}

TEST_P(OledlgTest, NoModeFlagLeavesListEmpty) {
    Dlg seen;
    user_ = [&] {
        seen = dlg_;
        command(1);
    };
    const GuestAddr io = make_io(0);
    EXPECT_EQ(insert(io), 1u);
    // Probe "flags 00000000" / "OK first item".
    EXPECT_EQ(seen.ctls[kIdcCreateNew].check, 0);
    EXPECT_EQ(seen.ctls[kIdcCreateFromFile].check, 0);
    EXPECT_TRUE(seen.ctls[kIdcObjectTypeList].items.empty());
    EXPECT_FALSE(seen.ctls[1].enabled);
    EXPECT_FALSE(seen.ctls[kIdcDisplayAsIcon].enabled);
    EXPECT_TRUE(clsid_untouched(io));
    EXPECT_EQ(memory_.read_u32(io + L().sc).value(), 0u);
    EXPECT_EQ(get_ptr(io + L().hmeta_pict), 0u);
    EXPECT_EQ(read_w(file_), "untouched");
}

TEST_P(OledlgTest, FromFileAndLink) {
    Dlg seen;
    user_ = [&] {
        seen = dlg_;
        ctl(kIdcFileDisplay).text = "C:\\Windows\\win.ini";
        ctl(kIdcLinkFile).check = 1;
        command(kIdcLinkFile);
        command(1);
    };
    GuestAddr io = make_io(kIofSelectCreateFromFile);
    EXPECT_EQ(insert(io), 1u);
    // Probe "flags 00000004": file radio, edit + Browse + link shown, list hidden.
    EXPECT_EQ(seen.ctls[kIdcCreateFromFile].check, 1);
    EXPECT_TRUE(seen.ctls[kIdcFileDisplay].visible);
    EXPECT_EQ(seen.ctls[kIdcFileDisplay].text, "untouched");
    EXPECT_TRUE(seen.ctls[kIdcFile].visible);
    EXPECT_TRUE(seen.ctls[kIdcLinkFile].visible);
    EXPECT_FALSE(seen.ctls[kIdcObjectTypeList].visible);
    EXPECT_TRUE(seen.ctls[1].enabled);
    // Probe "file link OK": lpszFile written, CHECKLINK added, clsid untouched.
    EXPECT_EQ(read_w(file_), "C:\\Windows\\win.ini");
    EXPECT_EQ(memory_.read_u32(io + L().flags).value(), kIofSelectCreateFromFile | kIofCheckLink);
    EXPECT_TRUE(clsid_untouched(io));
    EXPECT_EQ(memory_.read_u32(io + L().sc).value(), 0u);

    // Switching from Create New to a file (probe "file missing OK": no
    // existence check without IOF_CREATE*).
    user_ = [&] {
        command(kIdcCreateFromFile);
        ctl(kIdcFileDisplay).text = "C:\\vitreo_missing\\nothing.txt";
        command(1);
    };
    io = make_io(kIofSelectCreateNew);
    EXPECT_EQ(insert(io), 1u);
    EXPECT_EQ(read_w(file_), "C:\\vitreo_missing\\nothing.txt");
    EXPECT_EQ(memory_.read_u32(io + L().flags).value(), kIofSelectCreateNew);
}

TEST_P(OledlgTest, AnsiFromFileWritesAnsi) {
    user_ = [&] {
        ctl(kIdcFileDisplay).text = "C:\\Windows\\win.ini";
        command(1);
    };
    const GuestAddr io = make_io(kIofSelectCreateFromFile, false);
    EXPECT_EQ(insert(io, false), 1u);
    EXPECT_EQ(read_a(file_), "C:\\Windows\\win.ini");
}

TEST_P(OledlgTest, FlagStateTable) {
    Dlg seen;
    user_ = [&] { seen = dlg_; command(2); };
    auto run = [&](u32 flags) { (void)insert(make_io(flags)); return seen; };
    // IOF_SHOWHELP: Help shown and enabled.
    Dlg d = run(kIofShowHelp | kIofSelectCreateNew);
    EXPECT_TRUE(d.ctls[kIdcHelp].visible);
    EXPECT_TRUE(d.ctls[kIdcHelp].enabled);
    // DISABLELINK in file mode hides the link box.
    d = run(kIofSelectCreateFromFile | kIofDisableLink);
    EXPECT_FALSE(d.ctls[kIdcLinkFile].visible);
    EXPECT_FALSE(d.ctls[kIdcLinkFile].enabled);
    // CHECKLINK in file mode checks it.
    d = run(kIofSelectCreateFromFile | kIofCheckLink);
    EXPECT_EQ(d.ctls[kIdcLinkFile].check, 1);
    // DISABLEDISPLAYASICON hides the icon box.
    d = run(kIofSelectCreateNew | kIofDisableDisplayAsIcon);
    EXPECT_FALSE(d.ctls[kIdcDisplayAsIcon].visible);
    // HIDECHANGEICON destroys Change Icon (probe: "id 2105: absent").
    d = run(kIofSelectCreateNew | kIofHideChangeIcon);
    EXPECT_FALSE(d.ctls[kIdcChangeIcon].exists);
    // CHECKDISPLAYASICON shows Change Icon and the icon.
    d = run(kIofSelectCreateNew | kIofCheckDisplayAsIcon);
    EXPECT_EQ(d.ctls[kIdcDisplayAsIcon].check, 1);
    EXPECT_TRUE(d.ctls[kIdcChangeIcon].visible);
    EXPECT_TRUE(d.ctls[kIdcIconDisplay].visible);
    // SHOWINSERTCONTROL shows the third radio.
    d = run(kIofShowInsertControl);
    EXPECT_TRUE(d.ctls[kIdcInsertControl].visible);
    // SELECTCREATECONTROL: control list, Add Control, no icon box.
    d = run(kIofSelectCreateControl);
    EXPECT_EQ(d.ctls[kIdcInsertControl].check, 1);
    EXPECT_FALSE(d.ctls[kIdcInsertControl].visible);
    EXPECT_TRUE(d.ctls[kIdcAddControl].visible);
    EXPECT_FALSE(d.ctls[kIdcDisplayAsIcon].visible);
    ASSERT_EQ(d.ctls[kIdcObjectTypeList].items.size(), 1u);
    EXPECT_EQ(d.ctls[kIdcObjectTypeList].items[0], "VitreoProbe Control\t{6A1F0C11-0D2E-4B7A-9E51-3C1B7A2D000C}");
    EXPECT_TRUE(d.ctls[1].enabled);
    // Both SELECT flags: Create From File wins.
    d = run(kIofSelectCreateNew | kIofSelectCreateFromFile);
    EXPECT_EQ(d.ctls[kIdcCreateFromFile].check, 1);
    EXPECT_EQ(d.ctls[kIdcCreateNew].check, 0);
}

TEST_P(OledlgTest, CaptionAndCustomTemplate) {
    Dlg seen;
    user_ = [&] { seen = dlg_; command(2); };
    GuestAddr io = make_io(kIofSelectCreateNew);
    put_ptr(io + L().caption, wstr("Custom Caption"));
    EXPECT_EQ(insert(io), 2u);
    EXPECT_EQ(seen.title, "Custom Caption");

    // A custom template from hInstance + lpszTemplate (probe "template
    // MINIMAL": only OK and Cancel, still works).
    std::vector<u16> t = {0x00C0, 0x80C8, 0, 0, 2, 0, 0, 200, 60, 0, 0};
    for (char c : std::string("Minimal")) t.push_back(static_cast<u8>(c));
    t.push_back(0);
    t.push_back(8);
    for (char c : std::string("MS Shell Dlg")) t.push_back(static_cast<u8>(c));
    t.push_back(0);
    auto item = [&](u16 id, const char* text) {
        if (t.size() % 2) t.push_back(0);
        const u32 style = 0x50010000;
        t.insert(t.end(), {static_cast<u16>(style), static_cast<u16>(style >> 16), 0, 0, 140, 6, 50, 14, id, 0xFFFF, 0x80});
        for (const char* p = text; *p; ++p) t.push_back(static_cast<u8>(*p));
        t.push_back(0);
        t.push_back(0);
    };
    item(1, "OK");
    item(2, "Cancel");
    const GuestAddr tmpl = alloc(static_cast<u32>(t.size() * 2));
    for (usize i = 0; i < t.size(); ++i) (void)memory_.write_u16(tmpl + i * 2, t[i]);
    resources_["MINIMAL"] = tmpl;
    user_ = [&] { seen = dlg_; command(1); };
    io = make_io(kIofSelectCreateNew);
    put_ptr(io + L().hinstance, kModule);
    put_ptr(io + L().template_name, wstr("MINIMAL"));
    EXPECT_EQ(insert(io), 1u);
    EXPECT_EQ(seen.title, "Minimal");
    EXPECT_EQ(dialog_instance_, kModule);
    EXPECT_EQ(memory_.read_u32(io + L().sc).value(), 0u);
}

TEST_P(OledlgTest, WmCloseAndHookVeto) {
    user_ = [this] { message(kWmClose); };
    GuestAddr io = make_io(kIofSelectCreateNew);
    EXPECT_EQ(insert(io), 2u);
    EXPECT_TRUE(clsid_untouched(io));

    // A hook that returns nonzero for IDOK keeps the dialog open.
    hook_ = [](u32 msg, u64 wp, u64) -> u64 { return msg == kWmCommand && (wp & 0xFFFF) == 1 ? 1 : 0; };
    user_ = [this] {
        command(1);
        EXPECT_FALSE(dlg_.ended);
        command(2);
    };
    io = make_io(kIofSelectCreateNew);
    EXPECT_EQ(insert(io), 2u);
    EXPECT_TRUE(clsid_untouched(io));
}

TEST_P(OledlgTest, HelpPostsToOwner) {
    user_ = [this] { command(kIdcHelp); command(2); };
    (void)insert(make_io(kIofShowHelp | kIofSelectCreateNew));
    ASSERT_EQ(posted_.size(), 1u);
    EXPECT_EQ(posted_[0].hwnd, kOwner);
    EXPECT_EQ(posted_[0].msg, registered("OLEUI_MSG_HELP"));
    EXPECT_EQ(posted_[0].wp, kDialogHwnd);
    EXPECT_EQ(posted_[0].lp, kIddInsertObject);
}

TEST_P(OledlgTest, BrowseHookAndFileDialog) {
    u64 browse_wp = 0;
    std::string browse_text;
    hook_ = [&](u32 msg, u64 wp, u64 lp) -> u64 {
        if (msg == registered("OLEUI_MSG_BROWSE")) {
            browse_wp = wp;
            browse_text = read_w(static_cast<GuestAddr>(lp));
            return 0;
        }
        return 0;
    };
    open_file_ = "C:\\Windows\\win.ini";
    user_ = [this] {
        ctl(kIdcFileDisplay).text = "C:\\start.txt";
        command(kIdcFile);
        command(1);
    };
    const GuestAddr io = make_io(kIofSelectCreateFromFile);
    EXPECT_EQ(insert(io), 1u);
    // Probe "browse": MSG_BROWSE(wp=260, lp = the file buffer).
    EXPECT_EQ(browse_wp, kMaxPath);
    EXPECT_EQ(browse_text, "C:\\start.txt");
    EXPECT_TRUE(hook_saw(registered("OLEUI_MSG_BROWSE_OFN"), kIdBrowseInsertFile));
    ASSERT_NE(ofn_seen_, 0u);
    EXPECT_EQ(memory_.read_u32(ofn_seen_).value(), x64() ? 152u : 76u);
    EXPECT_EQ(read_w(file_), "C:\\Windows\\win.ini");

    // A hook that handles MSG_BROWSE suppresses the file dialog.
    ofn_seen_ = 0;
    hook_ = [&](u32 msg, u64, u64) -> u64 { return msg == registered("OLEUI_MSG_BROWSE") ? 1 : 0; };
    user_ = [this] { command(kIdcFile); command(2); };
    (void)insert(make_io(kIofSelectCreateFromFile));
    EXPECT_EQ(ofn_seen_, 0u);
}

TEST_P(OledlgTest, CreateNewObjectFailureIsScodeHasError) {
    register_ole32(0x80040154u, false);
    user_ = [this] { ASSERT_TRUE(select_item(kGood)); command(1); };
    const GuestAddr io = make_io(kIofSelectCreateNew | kIofCreateNewObject);
    const GuestAddr ppv = alloc(8);
    put_ptr(ppv, 0x9999);
    put_ptr(io + L().ppv_obj, ppv);
    // Probe "settled create new Good stg": 123, sc REGDB_E_CLASSNOTREG,
    // *ppvObj NULL, clsid written, ENDDIALOG still IDOK.
    EXPECT_EQ(insert(io), kIoErrScodeHasError);
    EXPECT_EQ(memory_.read_u32(io + L().sc).value(), 0x80040154u);
    EXPECT_EQ(get_ptr(ppv), 0u);
    EXPECT_EQ(get_guid(io + L().clsid), guid(kGood));
    EXPECT_EQ(created_clsid_, guid(kGood));
    EXPECT_TRUE(hook_saw(registered("OLEUI_MSG_ENDDIALOG"), 1));
}

TEST_P(OledlgTest, CreateNewObjectSuccessAndIcon) {
    register_ole32(0, true);
    user_ = [this] {
        ASSERT_TRUE(select_item(kGood));
        ctl(kIdcDisplayAsIcon).check = 1;
        command(kIdcDisplayAsIcon);
        command(1);
    };
    const GuestAddr io = make_io(kIofSelectCreateNew | kIofCreateNewObject);
    const GuestAddr ppv = alloc(8);
    put_ptr(io + L().ppv_obj, ppv);
    EXPECT_EQ(insert(io), 1u);
    EXPECT_EQ(memory_.read_u32(io + L().sc).value(), 0u);
    EXPECT_EQ(get_ptr(ppv), 0x0DEC0DE0u);
    // Probe "settled iconic Good OK": CHECKDISPLAYASICON added, hMetaPict set.
    EXPECT_EQ(memory_.read_u32(io + L().flags).value(),
              kIofSelectCreateNew | kIofCreateNewObject | kIofCheckDisplayAsIcon);
    EXPECT_EQ(get_ptr(io + L().hmeta_pict), 0x00ABC000u);
}

TEST_P(OledlgTest, CreateWithoutOle32IsNotImplemented) {
    user_ = [this] { ASSERT_TRUE(select_item(kGood)); command(1); };
    const GuestAddr io = make_io(kIofSelectCreateNew | kIofCreateNewObject);
    const GuestAddr ppv = alloc(8);
    put_ptr(ppv, 0x9999);
    put_ptr(io + L().ppv_obj, ppv);
    EXPECT_EQ(insert(io), kIoErrScodeHasError);
    EXPECT_EQ(memory_.read_u32(io + L().sc).value(), 0x80004001u);
    EXPECT_EQ(get_ptr(ppv), 0u);
}

// --- the other exports ------------------------------------------------------------

TEST_P(OledlgTest, OtherExportsFailWithDocumentedResults) {
    for (const char* name : {"OleUIPasteSpecialW", "OleUIPasteSpecialA", "OleUIEditLinksW", "OleUIEditLinksA",
                             "OleUIChangeIconW", "OleUIChangeIconA", "OleUIConvertW", "OleUIConvertA", "OleUIBusyW",
                             "OleUIBusyA", "OleUIObjectPropertiesW", "OleUIObjectPropertiesA",
                             "OleUIChangeSourceW", "OleUIChangeSourceA"}) {
        EXPECT_EQ(call_oledlg(name, {0}), 101u) << name;
        EXPECT_EQ(call_oledlg(name, {alloc(64)}), 112u) << name;
    }
    const GuestAddr out = alloc(8);
    put_ptr(out, 0x7777);
    EXPECT_EQ(call_oledlg("OleUIAddVerbMenuW", {0, 0, 0x100, 0, 1, 10, 0, 0, out}), 0u);
    EXPECT_EQ(get_ptr(out), 0u);
    EXPECT_EQ(call_oledlg("OleUICanConvertOrActivateAs", {alloc(16), 0, 0}), 0u);
    EXPECT_EQ(call_oledlg("OleUIUpdateLinksW", {0, 0, 0, 0}), 0u);
    EXPECT_EQ(call_oledlg("OleUIPromptUserW", {1008, 0}), 0xFFFFFFFFu);
}

INSTANTIATE_TEST_SUITE_P(Bitness, OledlgTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& i) { return i.param ? "x64" : "x86"; });

} // namespace
