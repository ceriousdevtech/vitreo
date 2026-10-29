#pragma once
/// @file guest_com.hpp
/// @brief Calling `IUnknown::AddRef` / `Release` on a COM interface the guest
/// owns, through the guest's own vtable.
///
/// Vitreo's graphics frontends hand out interfaces they implement themselves,
/// and for those a reference count is a host integer. But several APIs take an
/// interface the *guest* implements and require the object to hold a reference
/// on it: `IDXGIObject::SetPrivateDataInterface`,
/// `ID3D11DeviceChild::SetPrivateDataInterface`, and
/// `GetPrivateData` returning one of those. Storing such a pointer without the
/// COM side effects is how a frontend ends up reading freed guest memory.
///
/// Doing it properly means re-entering guest code, which needs the CPU state and
/// thunk registry that only the run loop owns -- so the call is injected rather
/// than reached for, the same way the presentation and swap-chain hooks are.

#include <vitreo/common.hpp>

#include <functional>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

/// Invoke a guest function with @p arg_count integer arguments, returning its
/// result. Injected by the host; an unset caller means the frontend must refuse
/// rather than pretend the call happened.
using GuestFunctionCaller =
    std::function<u64(GuestAddr function, const u64* args, usize arg_count)>;

/// IUnknown vtable slots.
inline constexpr usize COM_SLOT_QUERY_INTERFACE = 0;
inline constexpr usize COM_SLOT_ADD_REF = 1;
inline constexpr usize COM_SLOT_RELEASE = 2;

/// Call one IUnknown slot on @p interface_ptr through its own vtable, passing
/// the interface as `this`. Returns the guest's return value, or 0 when the
/// interface, its vtable, or the caller is unusable.
///
/// **Reentrancy — read before calling.** This runs arbitrary guest code. A
/// `Release` may drop the last reference, and the object's destructor may call
/// straight back into the API that is releasing it. So a caller must finish
/// every mutation of its own containers *before* calling, and must not hold an
/// iterator, reference or pointer into one across the call. A frontend that
/// releases an old interface while holding the map node it is about to
/// overwrite has written a use-after-free.
[[nodiscard]] u64 call_guest_com_slot(GuestMemory& memory, bool is_64bit,
                                      const GuestFunctionCaller& call,
                                      GuestAddr interface_ptr, usize slot);

/// `AddRef`. Returns the new reference count the guest reported.
[[nodiscard]] inline u32 guest_com_add_ref(GuestMemory& memory, bool is_64bit,
                                          const GuestFunctionCaller& call,
                                          GuestAddr interface_ptr) {
    return static_cast<u32>(call_guest_com_slot(
        memory, is_64bit, call, interface_ptr, COM_SLOT_ADD_REF));
}

/// `Release`. See the reentrancy note on call_guest_com_slot: this may run a
/// guest destructor that re-enters the caller.
[[nodiscard]] inline u32 guest_com_release(GuestMemory& memory, bool is_64bit,
                                          const GuestFunctionCaller& call,
                                          GuestAddr interface_ptr) {
    return static_cast<u32>(call_guest_com_slot(
        memory, is_64bit, call, interface_ptr, COM_SLOT_RELEASE));
}

} // namespace vitreo::winapi
