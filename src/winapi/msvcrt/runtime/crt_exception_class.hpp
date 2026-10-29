#pragma once
/// @file crt_exception_class.hpp
/// @brief msvcrt.dll's exported std::exception class (crt_exception_class.cpp).

#include "winapi/core/thunk_registry.hpp"

namespace vitreo { class GuestMemory; }

namespace vitreo::winapi {

class CrtMemoryManager;

/// Adds the exception constructors, copy, operator=, destructor, what() and
/// deleting destructors, x86 (thiscall) and x64 spellings.
void add_crt_exception_class_exports(DllExports& exports, CrtMemoryManager& mgr);

/// Writes exception's vftable, its RTTI locator and the "Unknown exception"
/// text into [area, area + area_size) and returns the vftable address to
/// publish as ??_7exception@@6B@ (0, with an UNIMPLEMENTED line, when the
/// slots' thunks are missing or the area does not fit). The msvcrt exports
/// must be registered first.
[[nodiscard]] GuestAddr publish_crt_exception_vftable(GuestMemory& memory, const ThunkRegistry& thunks,
                                                      bool is64, GuestAddr area, u32 area_size);

/// The vftable published last (0 before publish_crt_exception_vftable).
[[nodiscard]] GuestAddr crt_exception_vftable() noexcept;

} // namespace vitreo::winapi
