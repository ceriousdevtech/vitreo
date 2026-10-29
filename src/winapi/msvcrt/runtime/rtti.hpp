// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file rtti.hpp
/// @brief MSVC run-time type information helpers (`__RTDynamicCast` family).
///
/// These are the vcruntime entry points the compiler emits for `dynamic_cast`
/// and `typeid` on polymorphic types. They walk the RTTI graph the linker
/// placed next to each vftable: the complete-object locator at `vftable[-1]`,
/// its class-hierarchy descriptor, and the base-class descriptor array.
///
/// The generic missing-export stub answers these with 0xFFFFFFFF, which a
/// guest reads as a *successful* cast to a non-dereferenceable pointer, so a
/// failed `dynamic_cast` becomes a wild store instead of a NULL check.

namespace vitreo::winapi {

class Marshaler;

/// `void* __RTDynamicCast(void* inptr, LONG VfDelta, void* SrcType,
///                        void* TargetType, BOOL isReference)` (cdecl).
/// Returns the address of the `TargetType` subobject of the complete object
/// `inptr` belongs to, or NULL when the cast fails or is ambiguous.
void rt_dynamic_cast(Marshaler& m);

/// `void* __RTtypeid(void* inptr)` (cdecl) — the `TypeDescriptor*` of the
/// complete object's dynamic type.
void rt_typeid(Marshaler& m);

/// `void* __RTCastToVoid(void* inptr)` (cdecl) — `dynamic_cast<void*>`, i.e.
/// the address of the complete object.
void rt_cast_to_void(Marshaler& m);

/// `void* __AdjustPointer(void* ptr, const PMD& pmd)` (cdecl) — apply a
/// member-pointer displacement (member / vbtable / vbase-offset triple).
void adjust_pointer_export(Marshaler& m);

} // namespace vitreo::winapi
