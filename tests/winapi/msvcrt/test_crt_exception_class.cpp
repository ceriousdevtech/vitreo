/// @file test_crt_exception_class.cpp
/// @brief msvcrt.dll std::exception exports, the ??_7exception@@6B@ vftable
///        and __CxxFrameHandler, against
///        tools/oracle_probe/msvcrt_exception_initenv_probe.c's Windows output
///        (tests/oracle_data/msvcrt_exception_initenv_probe{,.x86}.expected).
///
/// The probe's exception rows are rebuilt here from the exports, in the
/// probe's order and wording, and compared line by line.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_exception_class.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kHeap = 0x20000000;
constexpr GuestAddr kData = 0x30000000;
constexpr GuestAddr kStack = 0x3000F000;
constexpr GuestAddr kRtti = 0x30008000;
constexpr GuestAddr kThunks = 0x70000000;

class CrtExceptionClassTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kData, 0x10000, MemoryProtection::ReadWrite).is_ok());
        mgr = std::make_unique<CrtMemoryManager>(memory, kHeap, 0x100000);
        ASSERT_TRUE(registry.register_dll("msvcrt.dll", create_crt_memory_thunks(*mgr)).is_ok());
        vftable = publish_crt_exception_vftable(memory, registry, x64(), kRtti, 0x160);
        ASSERT_NE(vftable, 0u);
    }
    bool x64() const { return GetParam(); }
    u32 ps() const { return x64() ? 8 : 4; }

    const char* name(const char* x86, const char* x64name) const { return x64() ? x64name : x86; }

    /// thiscall (x86: this in ECX, arguments on the stack) or x64 (RCX first).
    u64 call(const char* fn, GuestAddr self, std::vector<u64> args = {}) {
        const auto addr = registry.get_thunk_address("msvcrt.dll", fn);
        EXPECT_TRUE(addr.is_ok()) << fn;
        if (addr.is_err()) return 0;
        const auto handler = registry.resolve_handler(addr.value());
        EXPECT_TRUE(handler.is_ok()) << fn;
        cpu = cpu::CpuState{};
        if (x64()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = kStack;
        if (x64()) {
            cpu.gpr.rcx = self;
            if (!args.empty()) cpu.gpr.rdx = args[0];
            if (args.size() > 1) cpu.gpr.r8 = args[1];
        } else {
            cpu.gpr.rcx = self;
            for (usize i = 0; i < args.size(); ++i)
                EXPECT_TRUE(memory.write_u32(kStack + 4 + 4 * static_cast<u32>(i), static_cast<u32>(args[i])).is_ok());
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        (*handler.value())(m);
        return x64() ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax);
    }
    u64 ptr(GuestAddr at) {
        return x64() ? memory.read_u64(at).value_or(0) : memory.read_u32(at).value_or(0);
    }
    std::string cstr(GuestAddr at) {
        std::string s;
        for (u8 c; at && (c = memory.read_u8(at++).value_or(0)) != 0 && s.size() < 256;) s.push_back(static_cast<char>(c));
        return s;
    }
    GuestAddr obj(u32 index) {
        const GuestAddr o = kData + 0x100 + index * 0x20;
        for (u32 i = 0; i < 0x20; ++i) EXPECT_TRUE(memory.write_u8(o + i, 0xab).is_ok());
        return o;
    }
    GuestAddr text(const char* s) {
        const GuestAddr t = kData + 0x1000 + next_text;
        for (usize i = 0; i <= std::strlen(s); ++i) EXPECT_TRUE(memory.write_u8(t + i, static_cast<u8>(s[i])).is_ok());
        next_text += 0x40;
        return t;
    }
    GuestAddr ref_to(GuestAddr value) {
        const GuestAddr r = kData + 0x3000 + next_ref;
        next_ref += 8;
        EXPECT_TRUE((x64() ? memory.write_u64(r, value) : memory.write_u32(r, static_cast<u32>(value))).is_ok());
        return r;
    }
    std::string show(const std::string& label, GuestAddr o, GuestAddr src, u64 ret) {
        const u64 vf = ptr(o);
        const GuestAddr what = static_cast<GuestAddr>(ptr(o + ps()));
        std::string s = label + " ret=" + (ret == o ? "this" : "other") + " vfptr=" +
                        (vf == vftable ? "vftable" : "other") + " what=";
        if (what == 0) s += "null";
        else if (what == src) s += "same";
        else if (src && cstr(what) == cstr(src)) s += "copy";
        else s += "other";
        return s + " free=" + std::to_string(static_cast<i32>(memory.read_u32(o + 2 * ps()).value_or(0)));
    }
    std::string what_row(const std::string& label, GuestAddr o) {
        return label + " what='" + cstr(static_cast<GuestAddr>(call(name("?what@exception@@UBEPBDXZ",
                                                                         "?what@exception@@UEBAPEBDXZ"), o))) + "'";
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    ThunkRegistry registry{kThunks};
    std::unique_ptr<CrtMemoryManager> mgr;
    GuestAddr vftable = 0;
    u32 next_text = 0, next_ref = 0;
};

std::vector<std::string> expected_exception_rows(bool x64) {
    std::ifstream in(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "oracle_data" /
                     (x64 ? "msvcrt_exception_initenv_probe.expected" : "msvcrt_exception_initenv_probe.x86.expected"));
    EXPECT_TRUE(in.good());
    std::vector<std::string> rows;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (const char* p : {"ctor", "copy-", "assign-", "dtor ", "deleting0", "vftable[", "col "})
            if (line.rfind(p, 0) == 0) rows.push_back(line);
    }
    return rows;
}

TEST_P(CrtExceptionClassTest, EveryProbeRowMatchesWindows) {
    const char* ctor0 = name("??0exception@@QAE@XZ", "??0exception@@QEAA@XZ");
    const char* ctor1 = name("??0exception@@QAE@ABQBD@Z", "??0exception@@QEAA@AEBQEBD@Z");
    const char* ctor2 = name("??0exception@@QAE@ABQBDH@Z", "??0exception@@QEAA@AEBQEBDH@Z");
    const char* copy = name("??0exception@@QAE@ABV0@@Z", "??0exception@@QEAA@AEBV0@@Z");
    const char* assign = name("??4exception@@QAEAAV0@ABV0@@Z", "??4exception@@QEAAAEAV0@AEBV0@@Z");
    const char* dtor = name("??1exception@@UAE@XZ", "??1exception@@UEAA@XZ");
    const char* what = name("?what@exception@@UBEPBDXZ", "?what@exception@@UEBAPEBDXZ");

    std::vector<std::string> got;
    const auto what_addr = registry.get_thunk_address("msvcrt.dll", what);
    got.push_back(std::string("vftable[1]==what ") + (ptr(vftable + ps()) == what_addr.value() ? "1" : "0"));
    if (!x64()) {
        const auto g = registry.get_thunk_address("msvcrt.dll", "??_Gexception@@UAEPAXI@Z");
        const auto e = registry.get_thunk_address("msvcrt.dll", "??_Eexception@@UAEPAXI@Z");
        got.push_back(std::string("vftable[0]==??_G ") + (ptr(vftable) == g.value() ? "1" : "0") + " ==??_E " +
                      (ptr(vftable) == e.value() ? "1" : "0"));
    }
    {
        const GuestAddr col = static_cast<GuestAddr>(ptr(vftable - ps()));
        const u32 sig = memory.read_u32(col).value_or(9);
        const u32 td = memory.read_u32(col + 12).value_or(0);
        // x86: absolute; x64: image-relative with base = COL - pSelf.
        const GuestAddr base = x64() ? col - memory.read_u32(col + 20).value_or(0) : 0;
        got.push_back("col sig=" + std::to_string(sig) + " off=" + std::to_string(memory.read_u32(col + 4).value_or(9)) +
                      " cd=" + std::to_string(memory.read_u32(col + 8).value_or(9)) +
                      " type=" + cstr(base + td + 2 * ps()));
    }

    const GuestAddr probe_text = text("probe text");
    const GuestAddr text_ref = ref_to(probe_text);
    const GuestAddr a = obj(0), b = obj(1), c = obj(2), d = obj(3);
    got.push_back(show("ctor0", a, 0, call(ctor0, a)));
    got.push_back(what_row("ctor0", a));
    got.push_back(show("ctor1", b, probe_text, call(ctor1, b, {text_ref})));
    got.push_back(what_row("ctor1", b));
    got.push_back(show("ctor2", c, probe_text, call(ctor2, c, {text_ref, 1})));
    got.push_back(what_row("ctor2", c));
    got.push_back(show("ctor2-0", d, probe_text, call(ctor2, d, {text_ref, 0})));
    {
        const GuestAddr null_ref = ref_to(0);
        const GuestAddr e = obj(4), f = obj(5);
        got.push_back(show("ctor1-null", e, 0, call(ctor1, e, {null_ref})));
        got.push_back(what_row("ctor1-null", e));
        got.push_back(show("ctor2-null", f, 0, call(ctor2, f, {null_ref, 1})));
        got.push_back(what_row("ctor2-null", f));
    }
    const GuestAddr e = obj(6);
    got.push_back(show("copy-of-ctor1", e, static_cast<GuestAddr>(ptr(b + ps())), call(copy, e, {b})));
    got.push_back(what_row("copy-of-ctor1", e) + " srcsame=" + (ptr(e + ps()) == ptr(b + ps()) ? "1" : "0"));
    const GuestAddr f = obj(7);
    got.push_back(show("copy-of-ctor2", f, static_cast<GuestAddr>(ptr(c + ps())), call(copy, f, {c})));
    const GuestAddr g = obj(8);
    got.push_back(show("copy-of-ctor0", g, 0, call(copy, g, {a})));
    const GuestAddr h = obj(9);
    call(ctor1, h, {text_ref});
    got.push_back(show("assign-from-ctor2", h, static_cast<GuestAddr>(ptr(c + ps())), call(assign, h, {c})));
    const GuestAddr k = obj(10);
    call(ctor1, k, {ref_to(text("other text"))});
    got.push_back(show("assign-from-ctor1", k, static_cast<GuestAddr>(ptr(b + ps())), call(assign, k, {b})));
    got.push_back(std::string("assign-from-ctor1 srcsame=") + (ptr(k + ps()) == ptr(b + ps()) ? "1" : "0"));
    const GuestAddr self = obj(11);
    call(ctor1, self, {text_ref});
    const GuestAddr before = static_cast<GuestAddr>(ptr(self + ps()));
    got.push_back(show("assign-self", self, before, call(assign, self, {self})));
    call(dtor, b);
    got.push_back(std::string("dtor vfptr=") + (ptr(b) == vftable ? "vftable" : "other") + " what=" +
                  (ptr(b + ps()) ? "kept" : "null") + " free=" +
                  std::to_string(static_cast<i32>(memory.read_u32(b + 2 * ps()).value_or(0))));
    if (!x64()) {
        const GuestAddr z = obj(12);
        call(ctor1, z, {text_ref});
        const u64 r = call("??_Gexception@@UAEPAXI@Z", z, {0});
        got.push_back(std::string("deleting0 ret=") + (r == z ? "this" : "other") + " vfptr=" +
                      (ptr(z) == vftable ? "vftable" : "other"));
    }
    EXPECT_EQ(got, expected_exception_rows(x64()));
}

TEST_P(CrtExceptionClassTest, X64FrameHandlerContinuesSearch) {
    if (!x64()) GTEST_SKIP() << "x86 frame handling is covered by test_cxx_eh.cpp";
    EXPECT_EQ(call("__CxxFrameHandler", 0, {0, 0, 0}), 1u);  // ExceptionContinueSearch
}

TEST_P(CrtExceptionClassTest, FrameHandlerAliasesFrameHandler3OnX86) {
    if (x64()) GTEST_SKIP();
    // No FuncInfo / exception record: both personalities continue the search.
    cpu.gpr.rax = 0;
    EXPECT_EQ(call("__CxxFrameHandler", 0, {0, 0, 0}), 1u);
    EXPECT_EQ(call("__CxxFrameHandler3", 0, {0, 0, 0}), 1u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, CrtExceptionClassTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

} // namespace
