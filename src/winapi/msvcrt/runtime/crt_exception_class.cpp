/// @file crt_exception_class.cpp
/// @brief msvcrt.dll's exported std::exception class: constructors, copy,
///        assignment, destructor, what(), the deleting destructors and the
///        vftable data export ??_7exception@@6B@ with its RTTI locator.
///
/// Layout and behaviour measured on Windows 11 by
/// tools/oracle_probe/msvcrt_exception_initenv_probe.c (both bitnesses;
/// tests/oracle_data/msvcrt_exception_initenv_probe{,.x86}.expected):
///   object  = { vfptr; const char* _m_what; int _m_doFree; }
///   exception()                         -> what NULL, doFree 0
///   exception(const char* const& s)     -> a malloc'd copy of s (doFree 1),
///                                          NULL when s is NULL (doFree 0)
///   exception(const char* const& s,int) -> s itself, doFree 0 (int ignored)
///   exception(const exception& o)       -> a copy of o's text when o owns
///                                          it, else o's pointer
///   operator=(o)                        -> self-assignment unchanged; else
///                                          frees the own text, then as copy
///   ~exception()                        -> vfptr = exception's vftable; frees
///                                          the text it owns; fields kept
///   what()                              -> _m_what, or "Unknown exception"
///   vftable = { deleting destructor, what }, preceded by a Complete Object
///   Locator naming ".?AVexception@@" (x86 signature 0, absolute pointers;
///   x64 signature 1, image-relative offsets).

#include "winapi/msvcrt/runtime/crt_exception_class.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "core/memory/guest_memory.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {

std::atomic<u64> g_vftable{0};
std::atomic<u64> g_unknown_text{0};

constexpr const char* kDeleting32 = "??_Eexception@@UAEPAXI@Z";
constexpr const char* kScalarDeleting32 = "??_Gexception@@UAEPAXI@Z";
constexpr const char* kWhat32 = "?what@exception@@UBEPBDXZ";
constexpr const char* kDeleting64 = "??_Gexception@@UEAAPEAXI@Z";  // not exported by name
constexpr const char* kWhat64 = "?what@exception@@UEBAPEBDXZ";

struct Obj {
    Marshaler& m;
    GuestAddr self;
    u32 ps;
    GuestAddr what() const { return static_cast<GuestAddr>(m.read_ptr(self + ps).value_or(0)); }
    i32 do_free() const { return static_cast<i32>(m.read_u32(self + 2 * ps).value_or(0)); }
    void set(GuestAddr text, i32 owns) const {
        (void)m.write_ptr(self, g_vftable.load());
        (void)m.write_ptr(self + ps, text);
        (void)m.write_u32(self + 2 * ps, static_cast<u32>(owns));
    }
};

/// `this` (ECX for x86 thiscall, RCX for x64) and the explicit argument base.
GuestAddr self_of(Marshaler& m) {
    return m.cpu_is_64bit() ? m.arg_ptr(0) : static_cast<GuestAddr>(m.cpu()->gpr.rcx & 0xFFFFFFFFu);
}
usize first_arg(Marshaler& m) { return m.cpu_is_64bit() ? 1 : 0; }

GuestAddr duplicate(CrtMemoryManager& mgr, Marshaler& m, GuestAddr text) {
    const std::string s = m.read_ansi_string(text, 1u << 20).value_or("");
    const GuestAddr copy = mgr.crt_malloc(s.size() + 1);
    if (copy != 0) (void)m.write_ansi_string(copy, s);
    return copy;
}

void destroy(CrtMemoryManager& mgr, const Obj& o) {
    (void)o.m.write_ptr(o.self, g_vftable.load());
    if (o.do_free() != 0 && o.what() != 0) mgr.crt_free(o.what());
}

void copy_from(CrtMemoryManager& mgr, Marshaler& m, const Obj& dst, GuestAddr src_addr) {
    const Obj src{m, src_addr, dst.ps};
    const GuestAddr text = src.what();
    if (src.do_free() != 0 && text != 0) {
        const GuestAddr copy = duplicate(mgr, m, text);
        dst.set(copy, copy != 0 ? 1 : 0);
    } else {
        dst.set(text, 0);
    }
}

void put_u32(std::vector<u8>& b, usize at, u32 v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<u8>(v >> (8 * i));
}
void put_u64(std::vector<u8>& b, usize at, u64 v) {
    for (int i = 0; i < 8; ++i) b[at + i] = static_cast<u8>(v >> (8 * i));
}

} // namespace

void add_crt_exception_class_exports(DllExports& exports, CrtMemoryManager& mgr) {
    auto ctor0 = [](Marshaler& m) {
        const Obj o{m, self_of(m), m.cpu_is_64bit() ? 8u : 4u};
        o.set(0, 0);
        m.set_return(o.self);
    };
    auto ctor1 = [&mgr](Marshaler& m) {
        const Obj o{m, self_of(m), m.cpu_is_64bit() ? 8u : 4u};
        const GuestAddr ref = m.arg_ptr(first_arg(m));
        const GuestAddr text = static_cast<GuestAddr>(m.read_ptr(ref).value_or(0));
        if (text == 0) {
            o.set(0, 0);
        } else {
            const GuestAddr copy = duplicate(mgr, m, text);
            o.set(copy, copy != 0 ? 1 : 0);
        }
        m.set_return(o.self);
    };
    auto ctor2 = [](Marshaler& m) {
        const Obj o{m, self_of(m), m.cpu_is_64bit() ? 8u : 4u};
        const GuestAddr ref = m.arg_ptr(first_arg(m));
        o.set(static_cast<GuestAddr>(m.read_ptr(ref).value_or(0)), 0);
        m.set_return(o.self);
    };
    auto copy = [&mgr](Marshaler& m) {
        const Obj o{m, self_of(m), m.cpu_is_64bit() ? 8u : 4u};
        copy_from(mgr, m, o, m.arg_ptr(first_arg(m)));
        m.set_return(o.self);
    };
    auto assign = [&mgr](Marshaler& m) {
        const Obj o{m, self_of(m), m.cpu_is_64bit() ? 8u : 4u};
        const GuestAddr src = m.arg_ptr(first_arg(m));
        if (src != o.self) {
            destroy(mgr, o);
            copy_from(mgr, m, o, src);
        }
        m.set_return(o.self);
    };
    auto dtor = [&mgr](Marshaler& m) {
        destroy(mgr, Obj{m, self_of(m), m.cpu_is_64bit() ? 8u : 4u});
    };
    auto what = [](Marshaler& m) {
        const Obj o{m, self_of(m), m.cpu_is_64bit() ? 8u : 4u};
        const GuestAddr text = o.what();
        m.set_return(text != 0 ? text : g_unknown_text.load());
    };
    // void* exception::`deleting destructor'(unsigned flags): bit 0 frees the
    // storage; bit 1 (vector form) destroys the array whose element count
    // precedes it (MSVC's vector deleting destructor ABI).
    // The scalar form (??_G) only looks at bit 0.
    auto deleting_impl = [&mgr](Marshaler& m, bool vector) {
        const u32 ps = m.cpu_is_64bit() ? 8u : 4u;
        const GuestAddr self = self_of(m);
        const u32 flags = m.arg32(first_arg(m));
        const u32 size = ps == 8 ? 24u : 12u;  // sizeof(exception)
        if (vector && (flags & 2)) {
            const GuestAddr block = self - ps;
            const u64 count = ps == 8 ? m.read_u64(block).value_or(0) : m.read_u32(block).value_or(0);
            for (u64 i = count; i > 0; --i) destroy(mgr, Obj{m, self + static_cast<GuestAddr>((i - 1) * size), ps});
            if (flags & 1) mgr.crt_free(block);
            m.set_return(block);
            return;
        }
        destroy(mgr, Obj{m, self, ps});
        if (flags & 1) mgr.crt_free(self);
        m.set_return(self);
    };
    auto deleting = [deleting_impl](Marshaler& m) { deleting_impl(m, true); };
    auto scalar_deleting = [deleting_impl](Marshaler& m) { deleting_impl(m, false); };

    // x86: __thiscall, `this` in ECX, callee pops the stack arguments.
    exports.entries.push_back(ExportEntry{"??0exception@@QAE@XZ", 0, ctor0, 0});
    exports.entries.push_back(ExportEntry{"??0exception@@QAE@ABQBD@Z", 0, ctor1, 4});
    exports.entries.push_back(ExportEntry{"??0exception@@QAE@ABQBDH@Z", 0, ctor2, 8});
    exports.entries.push_back(ExportEntry{"??0exception@@QAE@ABV0@@Z", 0, copy, 4});
    exports.entries.push_back(ExportEntry{"??4exception@@QAEAAV0@ABV0@@Z", 0, assign, 4});
    exports.entries.push_back(ExportEntry{"??1exception@@UAE@XZ", 0, dtor, 0});
    exports.entries.push_back(ExportEntry{kWhat32, 0, what, 0});
    exports.entries.push_back(ExportEntry{kDeleting32, 0, deleting, 4});
    exports.entries.push_back(ExportEntry{kScalarDeleting32, 0, scalar_deleting, 4});
    // x64: `this` in RCX.
    exports.entries.push_back(ExportEntry{"??0exception@@QEAA@XZ", 0, ctor0});
    exports.entries.push_back(ExportEntry{"??0exception@@QEAA@AEBQEBD@Z", 0, ctor1});
    exports.entries.push_back(ExportEntry{"??0exception@@QEAA@AEBQEBDH@Z", 0, ctor2});
    exports.entries.push_back(ExportEntry{"??0exception@@QEAA@AEBV0@@Z", 0, copy});
    exports.entries.push_back(ExportEntry{"??4exception@@QEAAAEAV0@AEBV0@@Z", 0, assign});
    exports.entries.push_back(ExportEntry{"??1exception@@UEAA@XZ", 0, dtor});
    exports.entries.push_back(ExportEntry{kWhat64, 0, what});
    ExportEntry deleting64{kDeleting64, 0, scalar_deleting};
    deleting64.internal = true;  // x64 msvcrt does not export it; the vftable slot needs it
    exports.entries.push_back(std::move(deleting64));
}

GuestAddr publish_crt_exception_vftable(GuestMemory& memory, const ThunkRegistry& thunks, bool is64,
                                        GuestAddr area, u32 area_size) {
    const auto del = thunks.get_thunk_address("msvcrt.dll", is64 ? kDeleting64 : kDeleting32,
                                              /*include_internal=*/true);
    const auto what = thunks.get_thunk_address("msvcrt.dll", is64 ? kWhat64 : kWhat32);
    if (del.is_err() || what.is_err() || (is64 && area + area_size > 0xFFFFFFFFull)) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED msvcrt!??_7exception@@6B@: no %s\n",
                     del.is_err() || what.is_err() ? "thunk for the vftable slots" : "32-bit RTTI area");
        return 0;
    }
    // Pieces, in order: "Unknown exception", TypeDescriptor, BaseClassDescriptor,
    // BaseClassArray, ClassHierarchyDescriptor, Complete Object Locator, then
    // { &COL, deleting destructor, what } -- the vftable is the second slot.
    // x64 offsets are image-relative with an image base of 0 (COL.pSelf is
    // the COL's own address), which the area being below 4 GiB allows.
    const u32 ps = is64 ? 8 : 4;
    std::vector<u8> b(area_size, 0);
    usize at = 0;
    auto take = [&](usize n) { const usize r = at; at = (at + n + 7) & ~usize{7}; return r; };
    auto ref = [&](usize off) { return area + off; };
    auto put_ref = [&](usize field, usize target) {
        if (is64) put_u32(b, field, static_cast<u32>(ref(target)));
        else put_u32(b, field, static_cast<u32>(ref(target)));
    };
    const std::string unknown = "Unknown exception";
    const usize text = take(unknown.size() + 1);
    std::copy(unknown.begin(), unknown.end(), b.begin() + static_cast<std::ptrdiff_t>(text));
    const std::string name = ".?AVexception@@";
    // TypeDescriptor { pVFTable (type_info's vftable: not exported by Vitreo,
    // left NULL); spare; name[] }
    const usize td = take(2 * ps + name.size() + 1);
    std::copy(name.begin(), name.end(), b.begin() + static_cast<std::ptrdiff_t>(td + 2 * ps));
    // BaseClassDescriptor { pTypeDescriptor; numContainedBases; PMD{mdisp,
    // pdisp, vdisp}; attributes; [x64: pClassDescriptor] }
    const usize bcd = take(is64 ? 28 : 24);
    put_ref(bcd, td);
    put_u32(b, bcd + 8, 0);
    put_u32(b, bcd + 12, 0xFFFFFFFFu);  // pdisp -1: not a virtual base
    const usize bca = take(4);
    put_ref(bca, bcd);
    // ClassHierarchyDescriptor { signature; attributes; numBaseClasses; pBaseClassArray }
    const usize chd = take(16);
    put_u32(b, chd + 8, 1);
    put_ref(chd + 12, bca);
    if (is64) put_ref(bcd + 24, chd);
    // CompleteObjectLocator { signature; offset; cdOffset; pTypeDescriptor;
    // pClassDescriptor; [x64: pSelf] }
    const usize col = take(is64 ? 24 : 20);
    put_u32(b, col, is64 ? 1 : 0);
    put_ref(col + 12, td);
    put_ref(col + 16, chd);
    if (is64) put_ref(col + 20, col);
    const usize slots = take(3 * ps);
    if (is64) {
        put_u64(b, slots, ref(col));
        put_u64(b, slots + 8, del.value());
        put_u64(b, slots + 16, what.value());
    } else {
        put_u32(b, slots, static_cast<u32>(ref(col)));
        put_u32(b, slots + 4, static_cast<u32>(del.value()));
        put_u32(b, slots + 8, static_cast<u32>(what.value()));
    }
    if (at > area_size || memory.write_bytes(area, reinterpret_cast<const Byte*>(b.data()), b.size()).is_err()) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED msvcrt!??_7exception@@6B@: RTTI area too small\n");
        return 0;
    }
    g_unknown_text.store(ref(text));
    g_vftable.store(ref(slots + ps));
    return ref(slots + ps);
}

GuestAddr crt_exception_vftable() noexcept { return static_cast<GuestAddr>(g_vftable.load()); }

} // namespace vitreo::winapi
