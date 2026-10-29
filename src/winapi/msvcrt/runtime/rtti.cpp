// SPDX-License-Identifier: LGPL-2.1-only

/// @file rtti.cpp
/// @brief MSVC RTTI graph walk behind `dynamic_cast` / `typeid`.
///
/// Layout (MSVC, both bitnesses; on x64 every "pointer" field below is an
/// image-relative RVA and the image base is recovered from the locator's
/// `pSelf` field, which is the RVA of the locator itself):
///
///   vftable[-1] -> _RTTICompleteObjectLocator {
///       u32 signature;      // 0 = x86 (absolute), 1 = x64 (RVA)
///       u32 offset;         // offset of this vfptr inside the complete object
///       u32 cdOffset;       // constructor displacement
///       ptr pTypeDescriptor;
///       ptr pClassDescriptor;
///       u32 pSelf;          // x64 only
///   }
///   _RTTIClassHierarchyDescriptor {
///       u32 signature; u32 attributes; u32 numBaseClasses;
///       ptr pBaseClassArray;   // numBaseClasses entries, each a BCD pointer
///   }
///   _RTTIBaseClassDescriptor {
///       ptr pTypeDescriptor;
///       u32 numContainedBases;  // bases contained by this one, as a run that
///                               // immediately follows it in the array
///       i32 mdisp; i32 pdisp; i32 vdisp;   // PMD
///       u32 attributes;
///   }
///
/// The base-class array is stored so that a class is immediately followed by
/// its own `numContainedBases` bases; that ordering is what makes the
/// containment tests below exact rather than heuristic.

#include "winapi/msvcrt/runtime/rtti.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {

struct Pmd {
    i32 mdisp{0};
    i32 pdisp{-1};
    i32 vdisp{0};
};

struct BaseDesc {
    GuestAddr type_descriptor{0};
    u32       num_contained{0};
    Pmd       where{};
};

/// Reads of the RTTI graph must never fault the emulator: a guest can hand
/// `dynamic_cast` a pointer into freed or non-polymorphic memory, and Windows
/// answers that with a failed cast, not a crash.
struct RttiReader {
    Marshaler& m;
    bool       is64;
    GuestAddr  image_base{0};   ///< x64 only; 0 on x86 (fields are absolute)

    [[nodiscard]] usize ptr_size() const { return is64 ? 8u : 4u; }

    [[nodiscard]] std::optional<u32> u32_at(GuestAddr addr) const {
        auto r = m.read_u32(addr);
        if (r.is_err()) return std::nullopt;
        return r.value();
    }

    [[nodiscard]] std::optional<i32> i32_at(GuestAddr addr) const {
        auto r = u32_at(addr);
        if (!r) return std::nullopt;
        return static_cast<i32>(*r);
    }

    /// A real pointer field (vfptr slots, vbtable pointers) — never an RVA.
    [[nodiscard]] std::optional<GuestAddr> ptr_at(GuestAddr addr) const {
        auto r = m.read_ptr(addr);
        if (r.is_err()) return std::nullopt;
        return static_cast<GuestAddr>(r.value());
    }

    /// An RTTI cross-reference: absolute on x86, image-relative on x64.
    [[nodiscard]] std::optional<GuestAddr> ref_at(GuestAddr addr) const {
        if (!is64) return ptr_at(addr);
        auto rva = u32_at(addr);
        if (!rva) return std::nullopt;
        if (*rva == 0) return GuestAddr{0};
        return image_base + *rva;
    }

    /// Width of one RTTI cross-reference field.
    [[nodiscard]] usize ref_size() const { return is64 ? 4u : 4u; }
};

constexpr u32 COL_OFF_OFFSET      = 4;
constexpr u32 COL_OFF_TYPE_DESC   = 12;
constexpr u32 COL_OFF_CLASS_DESC  = 16;
constexpr u32 COL_OFF_SELF        = 20;   // x64 only

constexpr u32 CHD_OFF_NUM_BASES   = 8;
constexpr u32 CHD_OFF_BASE_ARRAY  = 12;

/// Locate the complete-object locator for the vfptr at `vfptr_addr` and,
/// on x64, recover the image base from its self-RVA.
struct Locator {
    GuestAddr address{0};
    u32       offset{0};
    GuestAddr type_descriptor{0};
    GuestAddr class_descriptor{0};
};

[[nodiscard]] std::optional<Locator> read_locator(RttiReader& r,
                                                  GuestAddr vfptr_addr) {
    auto vftable = r.ptr_at(vfptr_addr);
    if (!vftable || *vftable == 0) return std::nullopt;

    auto col_addr = r.ptr_at(*vftable - r.ptr_size());
    if (!col_addr || *col_addr == 0) return std::nullopt;

    Locator loc{};
    loc.address = *col_addr;

    if (r.is64) {
        // pSelf is the locator's own RVA, so base = address - pSelf. It is the
        // only way to anchor the RVA fields without asking the loader which
        // module this object's vftable came from.
        auto self_rva = r.u32_at(loc.address + COL_OFF_SELF);
        if (!self_rva || *self_rva > loc.address) return std::nullopt;
        r.image_base = loc.address - *self_rva;
    }

    auto offset = r.u32_at(loc.address + COL_OFF_OFFSET);
    if (!offset) return std::nullopt;
    loc.offset = *offset;

    auto type_desc = r.ref_at(loc.address + COL_OFF_TYPE_DESC);
    auto class_desc = r.ref_at(loc.address + COL_OFF_CLASS_DESC);
    if (!type_desc || !class_desc || *class_desc == 0) return std::nullopt;
    loc.type_descriptor = *type_desc;
    loc.class_descriptor = *class_desc;
    return loc;
}

/// Read the base-class descriptor array of `class_descriptor`, in array order.
[[nodiscard]] std::vector<BaseDesc> read_bases(const RttiReader& r,
                                               GuestAddr class_descriptor) {
    std::vector<BaseDesc> bases;
    auto count = r.u32_at(class_descriptor + CHD_OFF_NUM_BASES);
    auto array_addr = r.ref_at(class_descriptor + CHD_OFF_BASE_ARRAY);
    if (!count || !array_addr || *array_addr == 0) return bases;

    // A corrupt or non-RTTI pointer can decode as a huge count; cap the walk
    // rather than trusting guest data with an allocation.
    constexpr u32 MAX_BASES = 4096;
    const u32 n = *count > MAX_BASES ? MAX_BASES : *count;
    bases.reserve(n);

    for (u32 i = 0; i < n; ++i) {
        auto bcd = r.ref_at(*array_addr + i * r.ref_size());
        if (!bcd || *bcd == 0) break;

        auto type_desc = r.ref_at(*bcd);
        auto contained = r.u32_at(*bcd + r.ref_size());
        auto mdisp = r.i32_at(*bcd + r.ref_size() + 4);
        auto pdisp = r.i32_at(*bcd + r.ref_size() + 8);
        auto vdisp = r.i32_at(*bcd + r.ref_size() + 12);
        if (!type_desc || !contained || !mdisp || !pdisp || !vdisp) break;

        bases.push_back(BaseDesc{*type_desc, *contained,
                                 Pmd{*mdisp, *pdisp, *vdisp}});
    }
    return bases;
}

/// Apply a PMD to `obj`. With a non-negative `pdisp` the object holds a
/// virtual-base-table pointer at `obj + pdisp`, whose entry at `vdisp` is the
/// displacement from that vbptr slot to the virtual base.
[[nodiscard]] std::optional<GuestAddr> apply_pmd(const RttiReader& r,
                                                 GuestAddr obj,
                                                 const Pmd& pmd) {
    GuestAddr result = obj;
    if (pmd.pdisp >= 0) {
        const GuestAddr vbptr_slot =
            static_cast<GuestAddr>(static_cast<i64>(obj) + pmd.pdisp);
        auto vbtable = r.ptr_at(vbptr_slot);
        if (!vbtable || *vbtable == 0) return std::nullopt;
        auto vbase_off = r.i32_at(
            static_cast<GuestAddr>(static_cast<i64>(*vbtable) + pmd.vdisp));
        if (!vbase_off) return std::nullopt;
        result = static_cast<GuestAddr>(static_cast<i64>(vbptr_slot) +
                                        *vbase_off);
    }
    return static_cast<GuestAddr>(static_cast<i64>(result) + pmd.mdisp);
}

/// The decorated name of a TypeDescriptor: `{ void* vftable; void* spare;
/// char name[]; }`.
[[nodiscard]] std::string type_name(const RttiReader& r, GuestAddr type_desc) {
    if (type_desc == 0) return {};
    auto text = r.m.read_ansi_string(type_desc + 2 * r.ptr_size());
    if (text.is_err()) return {};
    return text.value();
}

/// Two TypeDescriptors denote the same type when they are the same object or
/// carry the same decorated name. The name comparison is what makes a cast
/// work across module boundaries: each DLL emits its own descriptor for a
/// shared type, so pointer identity alone fails every cross-DLL dynamic_cast.
[[nodiscard]] bool types_equal(const RttiReader& r, GuestAddr lhs,
                               GuestAddr rhs, const std::string& rhs_name) {
    if (lhs == rhs) return lhs != 0;
    if (lhs == 0 || rhs == 0 || rhs_name.empty()) return false;
    return type_name(r, lhs) == rhs_name;
}

/// Shared prologue: from the guest pointer plus its vfptr displacement, find
/// the locator and the address of the complete object.
struct ObjectView {
    Locator   locator{};
    GuestAddr complete{0};
};

[[nodiscard]] std::optional<ObjectView> view_object(RttiReader& r,
                                                    GuestAddr inptr,
                                                    i32 vf_delta) {
    if (inptr == 0) return std::nullopt;
    const GuestAddr vfptr_addr =
        static_cast<GuestAddr>(static_cast<i64>(inptr) + vf_delta);
    auto loc = read_locator(r, vfptr_addr);
    if (!loc) return std::nullopt;
    ObjectView v{};
    v.locator = *loc;
    // `offset` is where this vfptr sits inside the complete object.
    v.complete = static_cast<GuestAddr>(static_cast<i64>(vfptr_addr) -
                                        static_cast<i64>(loc->offset));
    return v;
}

} // namespace

void rt_dynamic_cast(Marshaler& m) {
    RttiReader r{m, m.cpu_is_64bit(), 0};

    const GuestAddr inptr = m.arg_ptr(0);
    const i32 vf_delta = static_cast<i32>(static_cast<u32>(m.arg(1)));
    const GuestAddr src_type = m.arg_ptr(2);
    const GuestAddr target_type = m.arg_ptr(3);

    if (inptr == 0) {
        m.set_return(0);
        return;
    }
    const std::string src_name = type_name(r, src_type);
    const std::string target_name = type_name(r, target_type);
    if (src_type != 0 && types_equal(r, src_type, target_type, target_name)) {
        m.set_return(inptr);
        return;
    }

    auto view = view_object(r, inptr, vf_delta);
    if (!view) {
        m.set_return(0);
        return;
    }

    const auto bases = read_bases(r, view->locator.class_descriptor);
    if (bases.empty()) {
        m.set_return(0);
        return;
    }

    // Address of each base subobject within the complete object.
    std::vector<GuestAddr> addr(bases.size(), 0);
    std::vector<bool> addr_ok(bases.size(), false);
    for (usize i = 0; i < bases.size(); ++i) {
        if (auto a = apply_pmd(r, view->complete, bases[i].where)) {
            addr[i] = *a;
            addr_ok[i] = true;
        }
    }

    // Upcast: if the static source type is a base in this hierarchy at exactly
    // `inptr`, the target may only be one of the bases it contains — the run of
    // `numContainedBases` entries that follows it.
    for (usize i = 0; i < bases.size(); ++i) {
        if (!addr_ok[i] || addr[i] != inptr ||
            !types_equal(r, bases[i].type_descriptor, src_type, src_name)) {
            continue;
        }
        const usize end =
            std::min(bases.size(), i + 1 + usize{bases[i].num_contained});
        for (usize j = i + 1; j < end; ++j) {
            if (addr_ok[j] && types_equal(r, bases[j].type_descriptor,
                                          target_type, target_name)) {
                m.set_return(addr[j]);
                return;
            }
        }
    }

    // Otherwise the cast is a downcast or a cross-cast, and the rule is the
    // language rule: it succeeds when the complete object contains exactly one
    // `TargetType` subobject. Two subobjects at the same address are one base
    // reached twice, not an ambiguity.
    std::vector<usize> candidates;
    for (usize i = 0; i < bases.size(); ++i) {
        if (addr_ok[i] && types_equal(r, bases[i].type_descriptor,
                                      target_type, target_name)) {
            candidates.push_back(i);
        }
    }
    auto unique_address = [&](const std::vector<usize>& set) -> GuestAddr {
        if (set.empty()) return 0;
        const GuestAddr first = addr[set.front()];
        for (usize i : set) {
            if (addr[i] != first) return 0;
        }
        return first;
    };

    if (GuestAddr unique = unique_address(candidates); unique != 0) {
        m.set_return(unique);
        return;
    }

    // Genuinely repeated base: keep only the occurrences that actually contain
    // the source subobject the guest handed us. Anything still ambiguous fails,
    // which is what Windows does rather than picking one.
    std::vector<usize> containing;
    for (usize i : candidates) {
        const usize end =
            std::min(bases.size(), i + 1 + usize{bases[i].num_contained});
        for (usize j = i + 1; j < end; ++j) {
            if (addr_ok[j] && addr[j] == inptr &&
                types_equal(r, bases[j].type_descriptor, src_type, src_name)) {
                containing.push_back(i);
                break;
            }
        }
    }
    m.set_return(unique_address(containing));
}

void rt_typeid(Marshaler& m) {
    RttiReader r{m, m.cpu_is_64bit(), 0};
    auto view = view_object(r, m.arg_ptr(0), 0);
    m.set_return(view ? view->locator.type_descriptor : GuestAddr{0});
}

void rt_cast_to_void(Marshaler& m) {
    RttiReader r{m, m.cpu_is_64bit(), 0};
    auto view = view_object(r, m.arg_ptr(0), 0);
    m.set_return(view ? view->complete : GuestAddr{0});
}

void adjust_pointer_export(Marshaler& m) {
    RttiReader r{m, m.cpu_is_64bit(), 0};
    const GuestAddr ptr = m.arg_ptr(0);
    const GuestAddr pmd_addr = m.arg_ptr(1);
    if (ptr == 0 || pmd_addr == 0) {
        m.set_return(ptr);
        return;
    }
    auto mdisp = r.i32_at(pmd_addr);
    auto pdisp = r.i32_at(pmd_addr + 4);
    auto vdisp = r.i32_at(pmd_addr + 8);
    if (!mdisp || !pdisp || !vdisp) {
        m.set_return(ptr);
        return;
    }
    auto adjusted = apply_pmd(r, ptr, Pmd{*mdisp, *pdisp, *vdisp});
    m.set_return(adjusted ? *adjusted : ptr);
}

} // namespace vitreo::winapi
